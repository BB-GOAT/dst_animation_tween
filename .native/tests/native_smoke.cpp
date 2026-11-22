// 隔离进程验证补帧核心、开关回退、三通道 relay 和发布逻辑；不执行游戏代码。
#include "../animation_tween.cpp"
#include <cassert>

const AnimFrame* forcedOriginal = nullptr;
const void* Original(const void* animation, int32_t, float) { return forcedOriginal ? forcedOriginal : animation; }

void CheckMatrixAxes() {
    // 独立构造旋转和非等比缩放；中间帧不应因缩放改变而扭曲旋转方向。
    const auto make = [](float sx, float sy, float angle, uint16_t* out) {
        out[0] = FloatToHalf(sx * cosf(angle)); out[1] = FloatToHalf(sx * sinf(angle));
        out[2] = FloatToHalf(-sy * sinf(angle)); out[3] = FloatToHalf(sy * cosf(angle));
        out[4] = FloatToHalf(12); out[5] = FloatToHalf(-4);
    };
    for (float sign : {1.f, -1.f}) for (bool rotate : {false, true}) {
        uint16_t first[6], second[6], out[6], reference[6];
        make(sign * 4, 1, .3f, first);
        make(sign * .25f, 3, rotate ? .9f : .3f, second);
        const auto plan = PrepareMatrix(first, second);
        assert(plan.mode == 2 || plan.mode == 3);
        for (unsigned step = 0; step <= 32; ++step) {
            const float t = step / 32.f, angle = .3f + (rotate ? .6f * t : 0);
            const float sx = sign * (4 - 3.75f * t), sy = 1 + 2 * t;
            const float expected[] = {sx*cosf(angle), sx*sinf(angle), -sy*sinf(angle), sy*cosf(angle),12,-4};
            ApplyMatrix(plan, t, out);
            assert(TweenElementMatrix(first, second, t, reference));
            for (unsigned k = 0; k < 6; ++k) {
                assert(fabsf(HalfToFloat(out[k]) - expected[k]) < .007f);
                assert(fabsf(HalfToFloat(reference[k]) - expected[k]) < .007f);
            }
        }
    }
    // 跨越正负pi边界仍走短弧；零缩放端点继续走线性分支。
    for (float sign : {1.f, -1.f}) {
        uint16_t first[6], second[6], out[6];
        make(sign*2, 1, 3.f, first); make(sign*3, 2, -3.f, second);
        const auto plan = PrepareMatrix(first, second);
        assert(plan.mode);
        ApplyMatrix(plan, .5f, out);
        assert(fabsf(HalfToFloat(out[0]) + sign*2.5f) < .005f);
        assert(fabsf(HalfToFloat(out[1])) < .005f);
    }
    {
        uint16_t first[6]{}, second[6], out[6];
        make(2, 3, .4f, second);
        const auto plan = PrepareMatrix(first, second);
        assert(plan.mode == 1);
        ApplyMatrix(plan, .5f, out);
        for (unsigned k=0; k<6; ++k)
            assert(fabsf(HalfToFloat(out[k]) - HalfToFloat(second[k])*.5f) < .005f);
    }
    // 客户端只读取得的国王魔杖cast_gem第39→40帧光束矩阵。
    // 原始两帧角度递减，旧列分解在前段反向；新输出必须连续同向。
    const float a[] = {-.4228515625f,-.74755859375f,3.751953125f,-2.12109375f,-39.34375f,18.890625f};
    const float b[] = {-.1553955078125f,-.179443359375f,3.421875f,-2.962890625f,-36.5f,29.6875f};
    uint16_t first[6], second[6], out[6];
    for (unsigned k = 0; k < 6; ++k) { first[k]=FloatToHalf(a[k]); second[k]=FloatToHalf(b[k]); }
    const auto plan = PrepareMatrix(first, second);
    assert(plan.mode);
    float previous = atan2f(a[1],a[0]);
    for (unsigned step = 1; step <= 128; ++step) {
        ApplyMatrix(plan, step / 128.f, out);
        const float angle = atan2f(HalfToFloat(out[1]),HalfToFloat(out[0]));
        assert(WrapAngleDiff(angle-previous) < .0005f);
        previous = angle;
    }
}

void CheckCore() {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    g_qpcInvFreq = 1.0 / static_cast<double>(frequency.QuadPart);
    g_origAnimGetFrame = Original;
    int token = 1;
    InterlockedExchange(&g_skeletalTweenEnabled, 0);
    assert(FastInterpGetFrameMain(&token, 0, 0, nullptr) == &token);
    uint16_t a[6] = {0x3c00,0,0,0x3c00,0,0};
    uint16_t b[6] = {0x3c00,0,0,0x3c00,0x5000,0}; // tx=32
    uint16_t out[6]{};
    assert(TweenElementMatrix(a,b,0.5f,out));
    assert(HalfToFloat(out[4]) == 16.0f);
    b[4] = FloatToHalf(500.0f);
    assert(!TweenElementMatrix(a,b,0.5f,out));
    b[4] = 0;
    b[0] = FloatToHalf(-1.0f);
    assert(!TweenElementMatrix(a,b,0.5f,out));
    AnimElement elements[2]{};
    for (auto& e : elements) {
        e.layer_hash=1; e.symbol_hash=2; e.frame_num=3;
        memcpy(e.m,a,sizeof(a));
    }
    elements[1].m[4] = FloatToHalf(32.0f);
    AnimFrame frames[2]{};
    for (unsigned i=0;i<2;++i) { frames[i].elements=&elements[i]; frames[i].num_elements=1; }
    Animation animation{};
    animation.frames=frames; animation.num_frames=2; animation.framerate=30;
    forcedOriginal = &frames[1];
    InterlockedExchange(&g_skeletalTweenEnabled, 1);
    assert(InterpGetFrameCore(&animation,1,0,0x1000,true) == &frames[0]);
    InterpGetFrameCore(&animation,1,1.0f/30,0x1000,true);
    // 固定在两次动画更新中点，避免调度延迟使测试落到原始端点帧。
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    bool found = false;
    for (auto& track : g_tlsAnimState->tracks) {
        if (track.valid && track.entity_key == 0x1000) {
            track.tick_period = 1.0f;
            track.tick_qpc = now.QuadPart - frequency.QuadPart / 2;
            found = true;
            break;
        }
    }
    assert(found);
    auto* result=static_cast<const AnimFrame*>(InterpGetFrameCore(&animation,1,1.0f/30,0x1000,true));
    assert(result && result->num_elements==1 && result->elements);
    const float x=HalfToFloat(result->elements[0].m[4]);
    assert(x>=0 && x<=32);
    assert(result != &frames[0] && result != &frames[1]);
    assert(ResolveFrameIdentity(result) == &frames[1]);
    assert(ResolveFrameIdentity(result) - frames == 1);
    assert(ResolveFrameIdentity(&frames[0]) == &frames[0]);
    assert(HalfToFloat(frames[0].elements[0].m[4]) == 0);
    assert(HalfToFloat(frames[1].elements[0].m[4]) == 32);
    // 关闭补帧不应使已经返回、尚待消费的插值帧失去身份。
    InterlockedExchange(&g_skeletalTweenEnabled,0);
    assert(ResolveFrameIdentity(result) == &frames[1]);
    forcedOriginal = nullptr;
}

void CheckUnsupportedFireflyFallback() {
    // 萤火虫新增光点后，同层同符号的数组下标会平移；贴图帧号也不是实例身份。
    AnimElement first[8]{}, second[9]{};
    for (auto* element : {&first[0], &first[1], &second[0], &second[1], &second[2]}) {
        element->layer_hash = 434069731;
        element->symbol_hash = 979885181;
        element->m[0] = element->m[3] = FloatToHalf(1.0f);
    }
    first[0].frame_num = 0;
    first[1].frame_num = 1;
    second[0].frame_num = 1;
    second[1].frame_num = 0;
    second[2].frame_num = 2;
    first[0].m[4] = FloatToHalf(-81.25f);
    first[1].m[4] = FloatToHalf(99.1f);
    second[0].m[4] = FloatToHalf(107.7f);
    second[1].m[4] = FloatToHalf(-80.35f);
    second[2].m[4] = FloatToHalf(92.5f);
    AnimFrame frames[2]{};
    frames[0].elements = first;
    frames[1].elements = second;
    Animation animation{};
    animation.frames = frames;
    animation.num_frames = 2;
    animation.framerate = 30.0f;
    forcedOriginal = &frames[1];
    InterlockedExchange(&g_skeletalTweenEnabled, 1);
    const LONG64 interpolatedBefore = g_animInterpFrames;
    // 覆盖新增、消失、同数换序，以及仅一侧有重复身份。
    const uint32_t counts[][2] = {{8,9},{8,1},{1,9},{8,8}};
    uintptr_t key = 0x2000;
    for (const auto& count : counts) {
        frames[0].num_elements = count[0];
        frames[1].num_elements = count[1];
        InterpGetFrameCore(&animation, 1, 0.0f, key, true);
        InterpGetFrameCore(&animation, 1, 1.0f / 30.0f, key, true);
        for (float phase : {0.0f, 0.005f, 0.5f, 0.995f, 1.0f}) {
            LARGE_INTEGER now{}, frequency{};
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&frequency);
            for (auto& track : g_tlsAnimState->tracks) {
                if (track.valid && track.entity_key == key) {
                    track.tick_period = 1.0f;
                    track.tick_qpc = now.QuadPart - static_cast<int64_t>(frequency.QuadPart * static_cast<double>(phase));
                }
            }
            const auto* result = static_cast<const AnimFrame*>(InterpGetFrameCore(&animation, 1, 1.0f / 30.0f, key, true));
            assert(result == forcedOriginal);
            assert(ResolveFrameIdentity(result) == forcedOriginal);
            assert(result->elements == second);
        }
        key += 0x100;
    }
    assert(g_animInterpFrames == interpolatedBefore);
    assert(HalfToFloat(first[0].m[4]) == -81.25f);
    assert(HalfToFloat(second[1].m[4]) == -80.375f);
    // 符号相同但层不同，或层相同但符号不同，仍具有唯一身份。
    frames[0].num_elements = 2;
    first[1].layer_hash += 1;
    assert(!HasRepeatedFireflyElements(frames[0]));
    first[1].layer_hash = first[0].layer_hash;
    first[1].symbol_hash += 1;
    assert(!HasRepeatedFireflyElements(frames[0]));
    forcedOriginal = nullptr;
    InterlockedExchange(&g_skeletalTweenEnabled, 0);
}

void CheckRepeatedRegularBones() {
    // 人物双腿等合法的重复部件必须继续补帧，不能整帧回退。
    AnimElement elements[2][2]{};
    AnimFrame frames[2]{};
    for (unsigned f = 0; f < 2; ++f) {
        frames[f].elements = elements[f];
        frames[f].num_elements = 2;
        for (unsigned i = 0; i < 2; ++i) {
            auto& element = elements[f][i];
            element.layer_hash = 1;
            element.symbol_hash = 2;
            element.frame_num = 3;
            element.m[0] = element.m[3] = FloatToHalf(1.0f);
            element.m[4] = FloatToHalf(i * 100.0f + f * 32.0f);
        }
        assert(!HasRepeatedFireflyElements(frames[f]));
    }
    Animation animation{};
    animation.frames = frames;
    animation.num_frames = 2;
    animation.framerate = 30.0f;
    forcedOriginal = &frames[1];
    InterlockedExchange(&g_skeletalTweenEnabled, 1);
    const LONG64 before = g_animElemsTweened;
    constexpr uintptr_t key = 0x4000;
    InterpGetFrameCore(&animation, 1, 0.0f, key, true);
    InterpGetFrameCore(&animation, 1, 1.0f / 30.0f, key, true);
    LARGE_INTEGER now{}, frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    for (auto& track : g_tlsAnimState->tracks) {
        if (track.valid && track.entity_key == key) {
            track.tick_period = 1.0f;
            track.tick_qpc = now.QuadPart - frequency.QuadPart / 2;
        }
    }
    const auto* result = static_cast<const AnimFrame*>(InterpGetFrameCore(&animation, 1, 1.0f / 30.0f, key, true));
    assert(result != &frames[0] && result != &frames[1]);
    assert(ResolveFrameIdentity(result) == &frames[1]);
    assert(fabsf(HalfToFloat(result->elements[0].m[4]) - 16.0f) < 0.1f);
    assert(fabsf(HalfToFloat(result->elements[1].m[4]) - 116.0f) < 0.1f);
    assert(g_animElemsTweened == before + 2);
    forcedOriginal = nullptr;
    InterlockedExchange(&g_skeletalTweenEnabled, 0);
}


void CheckOrdinaryCorrespondence() {
    const auto element = [](uint32_t sprite, float x, uint32_t layer = 1) {
        AnimElement e{};
        e.layer_hash = layer;
        e.symbol_hash = 2;
        e.frame_num = sprite;
        e.m[0] = e.m[3] = FloatToHalf(1);
        e.m[4] = FloatToHalf(x);
        return e;
    };
    const auto check = [](std::vector<AnimElement> first, std::vector<AnimElement> second,
                          const std::vector<int>& expected) {
        AnimFrame a{}, b{};
        a.elements = first.data(); a.num_elements = static_cast<uint32_t>(first.size());
        b.elements = second.data(); b.num_elements = static_cast<uint32_t>(second.size());
        assert(BuildOrdinaryCorrespondence(a, b) == expected);
        const auto plan = PreparePair(a, a, b);
        // 普通分支不改变输出元素数和源身份，未匹配者保持原矩阵。
        if (first.empty() || second.empty()) { assert(plan->matrices.empty()); return; }
        assert(!plan->trajectory && !plan->bypass && plan->sources.size() == first.size());
        for (size_t i = 0; i < first.size(); ++i) {
            assert(!plan->sources[i].target && plan->sources[i].index == i);
            if (expected[i] < 0) { assert(!plan->matrices[i].mode); continue; }
            const auto& end = second[expected[i]];
            const auto matrix = PrepareMatrix(first[i].m, end.m);
            assert(plan->matrices[i].mode == matrix.mode);
            assert(!memcmp(plan->matrices[i].v, matrix.v, sizeof(matrix.v)));
            assert(plan->targetSprite[i] == end.frame_num);
            assert(bool(plan->swaps[i]) == (first[i].frame_num != end.frame_num));
        }
    };
    // 旧算法会把两只脚都分配到目标0；任一源先换片也不能重复占用。
    check({element(1, 0), element(3, 100)}, {element(3, 2), element(3, 102)}, {0, 1});
    check({element(3, 0), element(1, 100)}, {element(3, 2), element(3, 102)}, {0, 1});
    // 连续位置对应不依赖片段分布或同片段的数组次序。
    check({element(1, 0), element(3, 100)}, {element(3, 102), element(1, 2)}, {1, 0});
    check({element(3, 0), element(3, 100)}, {element(3, 2), element(3, 102)}, {0, 1});
    // 数量变化时保留明确的连续对应，不把消失元素接到剩余目标。
    check({element(1, 0), element(3, 100)}, {element(3, 102)}, {-1, 0});
    check({element(1, 0), element(3, 100), element(5, 200)}, {element(3, 102), element(7, 2)}, {1, 0, -1});
    check({element(1, 0), element(3, 100, 9), element(3, 200)},
          {element(3, 2), element(3, 202), element(3, 102, 9)}, {0, 2, 1});
    // 静止换片及换序时，唯一的完整矩阵对应优先于片段和数组次序。
    check({element(5, 0), element(4, 100), element(16, 200), element(17, 300)},
          {element(6, 0), element(5, 100), element(17, 200), element(18, 300)}, {0, 1, 2, 3});
    check({element(1, 0), element(2, 100), element(3, 200)},
          {element(4, 100), element(2, 0), element(3, 202)}, {1, 0, 2});
    // 两端任一侧矩阵重复时无法确定身份，不强行接到另一个实例。
    check({element(1, 0), element(3, 0)}, {element(3, 0), element(3, 2)}, {-1, -1});
    check({element(1, 0), element(3, 100)}, {element(3, 0), element(3, 0)}, {-1, -1});
    // 新实例插入数组头部，以及末尾消失，不能平移既有实例的身份。
    check({element(2, 100)}, {element(2, -100), element(2, 102)}, {1});
    check({element(2, -100), element(2, 100)}, {element(2, 102)}, {-1, 0});
    // 深度变化导致前层减少、后层增加时，连续部件允许跨层，并保留目标层身份。
    check({element(0, 100, 1), element(0, 0, 1), element(0, -100, 9)},
          {element(0, 2, 1), element(0, 102, 9), element(0, -98, 9)}, {1, 0, 2});
    // 几何距离无法区分交叉者，或运动超出保守范围时，不伪造匹配。
    check({element(0, -10), element(0, 10)}, {element(0, 0), element(0, 1)}, {-1, -1});
    check({element(0, -100), element(0, 100)}, {element(0, -200), element(0, 200)}, {-1, -1});
    // 四倍间隔边界仍拒绝；仅移除不可能影响判据的更远候选。
    check({element(0, 0)}, {element(0, 40), element(0, 80)}, {-1});
    check({element(0, 0)}, {element(0, 40), element(0, 81)}, {0});
    check({}, {element(1, 0)}, {});
    check({element(1, 0)}, {}, {-1});
    // 穷举每组0至4个元素、三种贴片分布：检查一对一、同类和连续对应数。
    for (uint32_t n0 = 0; n0 <= 4; ++n0) for (uint32_t n1 = 0; n1 <= 4; ++n1) {
        uint32_t patterns0 = 1, patterns1 = 1;
        for (uint32_t i = 0; i < n0; ++i) patterns0 *= 3;
        for (uint32_t j = 0; j < n1; ++j) patterns1 *= 3;
        for (uint32_t p0 = 0; p0 < patterns0; ++p0) for (uint32_t p1 = 0; p1 < patterns1; ++p1) {
            std::vector<AnimElement> first(n0), second(n1);
            uint32_t bits = p0;
            for (uint32_t i = 0; i < n0; ++i) { first[i] = element(bits % 3, float(i * 10)); bits /= 3; }
            bits = p1;
            for (uint32_t j = 0; j < n1; ++j) { second[j] = element(bits % 3, float(j * 10 + 1)); bits /= 3; }
            AnimFrame a{}, b{};
            a.elements = first.data(); a.num_elements = n0;
            b.elements = second.data(); b.num_elements = n1;
            const auto mapping = BuildOrdinaryCorrespondence(a, b);
            std::vector<uint8_t> used(n1, 0);
            uint32_t count = 0;
            for (int j : mapping) if (j >= 0) {
                assert(static_cast<uint32_t>(j) < n1 && !used[j]); used[j] = 1; ++count;
            }
            assert(count == (std::min)(n0, n1));
        }
    }
}


void CheckEndpointTransitions() {
    // 前后层切换、重复元素换序、独立出生与消失不能改成跨实例的拉伸。
    AnimElement elements[2][4]{};
    const float x[2][4] = {{100, 0, -100, 300}, {2, 102, -98, -300}};
    const uint32_t layer[2][4] = {{1,1,9,20}, {1,9,9,30}};
    for (unsigned f=0; f<2; ++f) for (unsigned i=0; i<4; ++i) {
        auto& e=elements[f][i];e.symbol_hash=i==3?100+f:2;e.layer_hash=layer[f][i];
        e.m[0]=e.m[3]=FloatToHalf(1);e.m[4]=FloatToHalf(x[f][i]);
        e.meta0[4]=static_cast<uint8_t>(f*10+i);e.frame_num=f*10+i;
    }
    AnimElement saved[2][4];memcpy(saved,elements,sizeof(saved));
    AnimFrame frames[2]{};
    for(unsigned f=0;f<2;++f){frames[f].elements=elements[f];frames[f].num_elements=4;}
    Animation animation{};animation.frames=frames;animation.num_frames=2;animation.framerate=30;
    InvalidatePlans();InterlockedExchange(&g_skeletalTweenEnabled,1);forcedOriginal=&frames[1];
    constexpr uintptr_t key=0x13800;
    InterpGetFrameCore(&animation,0,0,key,true);
    LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);
    for(float phase:{.25f,.75f}){
        LARGE_INTEGER now{};QueryPerformanceCounter(&now);
        for(auto& tr:g_tlsAnimState->tracks)if(tr.valid&&tr.entity_key==key){
            tr.prev_f=0;tr.curr_f=1;tr.last_time=1.f/30;tr.tick_period=1000;
            tr.tick_qpc=now.QuadPart-static_cast<int64_t>(frequency.QuadPart*1000.0*phase);
        }
        BeginAnimDraw();const auto* out=static_cast<const AnimFrame*>(InterpGetFrameCore(&animation,0,1.f/30,key,true));
        assert(out && out->num_elements==4 && ResolveFrameIdentity(out)==&frames[1]);
        const auto& anchor=frames[phase<.5f?0:1];
        const float want0[]={100.5f,.5f,-99.5f,300}, want1[]={1.5f,101.5f,-98.5f,-300};
        for(unsigned i=0;i<4;++i){
            assert(!memcmp(&out->elements[i],&anchor.elements[i],offsetof(AnimElement,m)));
            assert(fabsf(HalfToFloat(out->elements[i].m[4])-(phase<.5f?want0[i]:want1[i]))<.1f);
        }
    }
    assert(!memcmp(saved,elements,sizeof(saved)));
    // 空端点在自己的半区间保持为空，不读取不存在的顺序表。
    frames[0].num_elements=0;InvalidatePlans();
    InterpGetFrameCore(&animation,0,0,key,true);
    for(float phase:{.25f,.75f}){
        LARGE_INTEGER now{};QueryPerformanceCounter(&now);
        for(auto& tr:g_tlsAnimState->tracks)if(tr.valid&&tr.entity_key==key){
            tr.prev_f=0;tr.curr_f=1;tr.last_time=1.f/30;tr.tick_period=1000;
            tr.tick_qpc=now.QuadPart-static_cast<int64_t>(frequency.QuadPart*1000.0*phase);
        }
        const auto* out=static_cast<const AnimFrame*>(InterpGetFrameCore(&animation,0,1.f/30,key,true));
        assert(out==&frames[phase<.5f?0:1]);
    }
    forcedOriginal=nullptr;InterlockedExchange(&g_skeletalTweenEnabled,0);InvalidatePlans();
}

void CheckFrameGeometry() {
    FrameGeometryLayout verified;
    assert(VerifyFrameGeometryLayout(kFrameGeometryBranch, sizeof(kFrameGeometryBranch), verified) && verified.verified);
    std::vector<uint8_t> branch(kFrameGeometryBranch, kFrameGeometryBranch + sizeof(kFrameGeometryBranch));
    branch.insert(branch.end(), kFrameGeometryBranch, kFrameGeometryBranch + sizeof(kFrameGeometryBranch));
    assert(!VerifyFrameGeometryLayout(branch.data(), branch.size(), verified) && !verified.verified);
    branch.resize(sizeof(kFrameGeometryBranch)); branch[0] ^= 1;
    assert(!VerifyFrameGeometryLayout(branch.data(), branch.size(), verified));
    std::array<uint8_t, 0x200> command{};
    std::array<uint8_t, 0xC0> build{};
    std::array<uint8_t, 16> symbolOverride{};
    const auto store = [](auto& bytes, size_t at, auto value) { memcpy(bytes.data() + at, &value, sizeof(value)); };
    const uintptr_t key = 0xB000;
    store(command, 8, key);
    store(command, 0xB8, build.data());
    store(build, 0xB0, uintptr_t(1));
    g_frameGeometry.verified = true;
    assert(UsesFrameGeometry(command.data()));
    store(command, 0x170, uintptr_t(1));
    assert(!UsesFrameGeometry(command.data()));
    store(command, 0x170, uintptr_t(0));
    store(command, 0x148, symbolOverride.data());
    store(command, 0x150, symbolOverride.data() + symbolOverride.size());
    assert(!UsesFrameGeometry(command.data()));
    store(symbolOverride, 0, uint32_t(0x1D298B03));
    assert(UsesFrameGeometry(command.data()));
    store(command, 0x150, symbolOverride.data() + 8);
    assert(!UsesFrameGeometry(command.data()));
    store(command, 0x148, uintptr_t(0));
    store(command, 0x150, uintptr_t(0));
    g_frameGeometry.verified = false;
    assert(!UsesFrameGeometry(command.data()));
    g_frameGeometry.verified = true;

    const auto element = [](uint32_t layer, uint32_t sprite, float x, uint8_t meta) {
        AnimElement e{};
        e.layer_hash = layer; e.symbol_hash = 2; e.frame_num = sprite;
        e.m[0] = e.m[3] = FloatToHalf(1); e.m[4] = FloatToHalf(x); e.meta0[4] = meta;
        return e;
    };
    std::vector<AnimElement> first{element(1,1,0,10), element(2,2,100,11), element(1,3,200,12)};
    std::vector<AnimElement> second{element(2,2,132,20), element(1,3,232,21), element(1,1,32,22), element(9,0,300,23)};
    const auto originalFirst = first, originalSecond = second;
    AnimFrame frames[2]{};
    frames[0].elements = first.data(); frames[0].num_elements = 3;
    frames[1].elements = second.data(); frames[1].num_elements = 4;
    Animation animation{}; animation.frames = frames; animation.num_frames = 2; animation.framerate = 30;
    forcedOriginal = &frames[1];
    InvalidatePlans(); InterlockedExchange(&g_skeletalTweenEnabled, 1);
    FastInterpGetFrameMain(&animation, 1, 0, command.data());
    FastInterpGetFrameMain(&animation, 1, 1.f/30, command.data());
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
    for (float phase : {.25f, .75f}) {
        LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
        for (auto& track : g_tlsAnimState->tracks) if (track.valid && track.entity_key == key) {
            track.tick_period = 1;
            track.tick_qpc = now.QuadPart - static_cast<int64_t>(frequency.QuadPart * phase);
        }
        const auto* result = static_cast<const AnimFrame*>(FastInterpGetFrameMain(&animation, 1, 1.f/30, command.data()));
        const auto& anchor = frames[phase < .5f ? 0 : 1];
        assert(result != &anchor && ResolveFrameIdentity(result) == &anchor);
        assert(result->num_elements == anchor.num_elements);
        const float expectedFirst[] = {8,108,208}, expectedSecond[] = {124,224,24,300};
        for (uint32_t i = 0; i < result->num_elements; ++i) {
            assert(result->elements[i].layer_hash == anchor.elements[i].layer_hash);
            assert(result->elements[i].symbol_hash == anchor.elements[i].symbol_hash);
            assert(result->elements[i].frame_num == anchor.elements[i].frame_num);
            assert(result->elements[i].meta0[4] == anchor.elements[i].meta0[4]);
            assert(fabsf(HalfToFloat(result->elements[i].m[4]) - (phase < .5f ? expectedFirst[i] : expectedSecond[i])) < .1f);
        }
    }
    assert(!memcmp(first.data(), originalFirst.data(), first.size() * sizeof(AnimElement)));
    assert(!memcmp(second.data(), originalSecond.data(), second.size() * sizeof(AnimElement)));
    InterlockedExchange(&g_skeletalTweenEnabled, 0);
    assert(FastInterpGetFrameMain(&animation, 1, 1.f/30, command.data()) == forcedOriginal);
    assert(g_tlsLastParentKey == key);
    uint64_t stats[6]{};
    assert(HS_ReadStats(stats,6) && stats[5] >= 2);
    forcedOriginal = nullptr; g_frameGeometry.verified = false; InvalidatePlans();
}

void CheckNestedFrames() {
    AnimElement elements[2]{};
    for (auto& e : elements) {
        e.layer_hash = 1; e.symbol_hash = 2;
        e.m[0] = e.m[3] = FloatToHalf(1);
    }
    elements[1].m[4] = FloatToHalf(32);
    AnimFrame frames[2]{};
    for (unsigned i = 0; i < 2; ++i) { frames[i].elements = &elements[i]; frames[i].num_elements = 1; }
    Animation animation{}; animation.frames = frames; animation.num_frames = 2; animation.framerate = 30;
    forcedOriginal = &frames[1];
    InvalidatePlans(); InterlockedExchange(&g_skeletalTweenEnabled, 1);
    BeginAnimDraw(); g_tlsLastParentKey = 0xC000;
    std::array<uint8_t, 0x120 * 10> items{};
    std::array<uint8_t, 0x40> node{};
    const uintptr_t begin = reinterpret_cast<uintptr_t>(items.data()), end = begin + items.size();
    const uint32_t symbol = 19;
    memcpy(node.data() + 0x18, &symbol, 4);
    memcpy(node.data() + 0x30, &begin, 8); memcpy(node.data() + 0x38, &end, 8);
    std::vector<uintptr_t> keys;
    std::vector<const AnimFrame*> results;
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
    for (unsigned i = 0; i < 10; ++i) {
        const OverlayDrawContext draw{&items[i * 0x120], node.data()};
        const uintptr_t key = i ? OverlayTrackKey(&animation, draw) : g_tlsLastParentKey;
        assert(std::find(keys.begin(), keys.end(), key) == keys.end()); keys.push_back(key);
        const auto call = [&](float time) {
            return static_cast<const AnimFrame*>(i ? FastInterpGetFrameOverlay(&animation, 1, time, &draw)
                : InterpGetFrameCore(&animation, 1, time, key, true));
        };
        call(0); call(1.f/30);
        LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
        for (auto& track : g_tlsAnimState->tracks) if (track.valid && track.entity_key == key) {
            track.tick_period = 1000;
            track.tick_qpc = now.QuadPart - frequency.QuadPart * (50 + i * 90);
        }
        results.push_back(call(1.f/30));
    }
    for (unsigned i = 0; i < results.size(); ++i) {
        assert(results[i] != &frames[0] && results[i] != &frames[1]);
        assert(fabsf(HalfToFloat(results[i]->elements[0].m[4]) - 32 * (.05f + .09f * i)) < .05f);
        assert(ResolveFrameIdentity(results[i]) == &frames[1]);
        for (unsigned j = 0; j < i; ++j) assert(results[i] != results[j]);
    }
    const auto allocated = g_tlsAnimState->slots.size();
    const auto* firstSlot = &g_tlsAnimState->slots[0]->frame;
    BeginAnimDraw();
    assert(g_tlsAnimState->ring == 0 && g_tlsAnimState->slots.size() == allocated);
    assert(&g_tlsAnimState->NextSlot().frame == firstSlot);
    InterlockedExchange(&g_skeletalTweenEnabled, 0); forcedOriginal = nullptr; InvalidatePlans();
}

void CheckMenuBatGroups() {
    std::vector<AnimElement> elements[3];
    AnimFrame frames[3]{};
    const auto add = [](std::vector<AnimElement>& out, float x, float y, uint32_t sprite) {
        for (uint32_t part=0; part<3; ++part) {
            AnimElement e{};
            e.layer_hash=kMenuBatLayer; e.symbol_hash=part?kMenuBatWing:kMenuBatBody;
            e.frame_num=part?sprite:0;
            e.m[0]=FloatToHalf(part==1?-1.0f:1.0f); e.m[3]=FloatToHalf(1);
            e.m[4]=FloatToHalf(x+(part==1?-4.0f:part==2?4.0f:0.0f)); e.m[5]=FloatToHalf(y);
            out.push_back(e);
        }
    };
    add(elements[0],0,0,0);
    add(elements[1],-10,70,0); add(elements[1],10,0,0);
    add(elements[2],0,70,0); add(elements[2],20,0,1);
    for(unsigned f=0;f<3;++f) {
        frames[f].elements=elements[f].data(); frames[f].num_elements=static_cast<uint32_t>(elements[f].size());
    }
    std::vector<int> mapping(3,-1);
    std::vector<uint8_t> swaps(3),fade(3),birth(6);
    std::vector<MatrixPlan> coherent;
    assert(MapMenuBats(frames[0],frames[0],frames[1],mapping,swaps,fade,birth,coherent));
    assert((mapping==std::vector<int>{3,4,5}));
    assert(birth[0]&&birth[1]&&birth[2]&&!birth[3]);
    auto plan=PreparePair(frames[0],frames[1],frames[2]);
    assert(plan&&!plan->bypass&&plan->trajectory&&plan->matrices.size()==6);
    // 后一只换翼贴图时仍跟随后一只本体，不能匹配前一只同贴图片段。
    for(unsigned i=3;i<6;++i) {
        uint16_t out[6]{}; ApplyMatrix(plan->matrices[i],.5f,out);
        assert(fabsf(HalfToFloat(out[4])-(15.0f+(i==4?-4.0f:i==5?4.0f:0.0f)))<.1f);
    }
    // 一片翼触发旋转护栏时，整组统一位移并保留贴图和相对位置。
    elements[2][4].m[0]=FloatToHalf(1);
    plan=PreparePair(frames[0],frames[1],frames[2]);
    for(unsigned i=3;i<6;++i) {
        assert(plan->matrices[i].mode==1&&!plan->swaps[i]);
        uint16_t out[6]{}; ApplyMatrix(plan->matrices[i],.5f,out);
        assert(fabsf(HalfToFloat(out[4])-HalfToFloat(elements[1][i].m[4])-5.0f)<.1f);
    }
    // 未核实的组结构不允许猜测；其它层的同名符号不受此规则影响。
    elements[2][4].symbol_hash=kMenuBatBody;
    assert(PreparePair(frames[0],frames[1],frames[2])->bypass);
    for(auto& e:elements[2])++e.layer_hash;
    assert(!ContainsMenuBat(frames[2]));
}

void CheckLocalPlans() {
    AnimElement elements[3][3]{};
    AnimFrame frames[3]{};
    const float positions[3][3] = {{-3,3,0},{-1,1,0},{1,-1,0}};
    for (unsigned f=0; f<3; ++f) {
        frames[f].elements=elements[f]; frames[f].num_elements=2;
        for (unsigned i=0; i<3; ++i) {
            auto& e=elements[f][i]; e.layer_hash=kFireflyLayer; e.symbol_hash=kFireflySymbol;
            e.m[0]=e.m[3]=FloatToHalf(1); e.m[4]=FloatToHalf(positions[f][i]);
        }
    }
    // 两个光点交叉时保留速度方向；仅按最近距离会对调实例。
    auto mapping=BuildLocalCorrespondence(frames[0],frames[1],frames[2]);
    assert((mapping==std::vector<int>{0,1}));
    Animation animation{}; animation.frames=frames; animation.num_frames=3; animation.framerate=30;
    InvalidatePlans();
    const auto* plan=LookupPair(&animation,1,frames[0],frames[1],frames[2]);
    assert(plan && plan->trajectory && !plan->bypass && plan->tweened==2);
    uint64_t builds=g_pairCache.builds;
    assert(LookupPair(&animation,1,frames[0],frames[1],frames[2])==plan);
    assert(g_pairCache.builds==builds);
    // 同地址、同元素数量，前一帧的内容改变也必须重建。
    elements[0][0].m[4]=FloatToHalf(-1); elements[0][1].m[4]=FloatToHalf(1);
    plan=LookupPair(&animation,1,frames[0],frames[1],frames[2]);
    assert(g_pairCache.builds==++builds);
    mapping=BuildLocalCorrespondence(frames[0],frames[1],frames[2]);
    assert((mapping==std::vector<int>{1,0}));
    // 同地址原地修改下一帧、meta指针或元素数量，不能沿用旧输入。
    elements[2][0].m[4]=FloatToHalf(4);
    assert(LookupPair(&animation,1,frames[0],frames[1],frames[2]));
    assert(g_pairCache.builds==++builds);
    elements[1][0].meta0[4]^=1;
    assert(LookupPair(&animation,1,frames[0],frames[1],frames[2]));
    assert(g_pairCache.builds==++builds);
    frames[2].num_elements=3;
    assert(LookupPair(&animation,1,frames[0],frames[1],frames[2]));
    assert(g_pairCache.builds==++builds);
    InvalidatePlans();
    assert(LookupPair(&animation,1,frames[0],frames[1],frames[2]));
    assert(g_pairCache.builds==++builds && g_pairCache.entries.size()==1);
    // 出生点插回目标顺序，且零缩放端点不引用旧快照中的meta。
    const float birthPositions[3][3]={{-100,0,0},{-98,2,0},{-96,100,4}};
    for (unsigned f=0;f<3;++f) for(unsigned i=0;i<3;++i) elements[f][i].m[4]=FloatToHalf(birthPositions[f][i]);
    plan=LookupPair(&animation,1,frames[0],frames[1],frames[2]);
    assert(plan->matrices.size()==3 && plan->sources[1].target && plan->sources[1].index==1);
    uint16_t out[6]{};
    ApplyMatrix(plan->matrices[1],0,out);assert(HalfToFloat(out[0])==0);
    ApplyMatrix(plan->matrices[1],1,out);assert(HalfToFloat(out[0])==1);
    // 预算淘汰只影响命中率，不改变结果；每个键均可重新计算。
    for(uint32_t i=0;i<kPlanEntryLimit+2;++i) assert(LookupPair(&animation,i,frames[0],frames[1],frames[2]));
    assert(g_pairCache.entries.size()<=kPlanEntryLimit && g_pairCache.bytes<=kPlanBudget);
    assert(LookupPair(&animation,1,frames[0],frames[1],frames[2])->sources[1].target);
    // 大帧对覆盖字节预算，不能只验证条目数限制。
    std::vector<AnimElement> large0(512),large1(512);
    for(unsigned i=0;i<512;++i) {
        large0[i].layer_hash=large1[i].layer_hash=i+1;
        large0[i].symbol_hash=large1[i].symbol_hash=2;
        large0[i].m[0]=large0[i].m[3]=large1[i].m[0]=large1[i].m[3]=FloatToHalf(1);
        large1[i].m[4]=FloatToHalf(2);
    }
    AnimFrame a{},b{};a.elements=large0.data();b.elements=large1.data();a.num_elements=b.num_elements=512;
    g_pairCache.Clear();
    for(uint32_t i=0;i<300;++i) {
        const auto* large=LookupPair(&animation,i,a,a,b);
        assert(large && large->tweened==512 && g_pairCache.bytes<=kPlanBudget);
    }
    assert(g_pairCache.entries.size()<300);
    InvalidatePlans();
}

void CheckFireflyInterpolation() {
    AnimElement elements[3][3]{};
    AnimFrame frames[3]{};
    const float positions[3][3] = {{-100,0,0},{-98,2,0},{-96,100,4}};
    for(unsigned f=0;f<3;++f) {
        frames[f].elements=elements[f]; frames[f].num_elements=f==2?3:2;
        for(unsigned i=0;i<3;++i) {
            auto& e=elements[f][i]; e.layer_hash=kFireflyLayer; e.symbol_hash=kFireflySymbol;
            e.m[0]=e.m[3]=FloatToHalf(1);e.m[4]=FloatToHalf(positions[f][i]);
            e.meta0[4]=static_cast<uint8_t>(f*10+i);
        }
    }
    AnimElement original[3][3];memcpy(original,elements,sizeof(original));
    Animation animation{};animation.frames=frames;animation.num_frames=3;animation.framerate=30;
    forcedOriginal=&frames[2];
    InterlockedExchange(&g_skeletalTweenEnabled,1);InvalidatePlans();
    constexpr uintptr_t key=0x9000;
    InterpGetFrameCore(&animation,1,0,key,true);
    LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);
    for(float phase:{0.0f,.25f,.5f,.75f,1.0f}) {
        LARGE_INTEGER now{};QueryPerformanceCounter(&now);
        for(auto& track:g_tlsAnimState->tracks) if(track.valid && track.entity_key==key) {
            track.last_time=2.f/30;track.prev_f=1;track.curr_f=2;track.tick_period=1;
            track.tick_qpc=now.QuadPart-static_cast<int64_t>(frequency.QuadPart*phase);
        }
        const auto* result=static_cast<const AnimFrame*>(InterpGetFrameCore(&animation,1,2.f/30,key,true));
        if(phase==0) {assert(result==&frames[1]);continue;}
        if(phase==1) {assert(result==&frames[2]);continue;}
        assert(result->num_elements==3 && ResolveFrameIdentity(result)==&frames[2]);
        assert(fabsf(HalfToFloat(result->elements[0].m[4])-(-98+2*phase))<.1f);
        assert(fabsf(HalfToFloat(result->elements[2].m[4])-(2+2*phase))<.1f);
        assert(HalfToFloat(result->elements[1].m[4])==100);
        assert(fabsf(HalfToFloat(result->elements[1].m[0])-phase)<.001f);
        assert(result->elements[0].meta0[4]==10 && result->elements[1].meta0[4]==21);
    }
    assert(!memcmp(original,elements,sizeof(original)));
    // 零元素一端仍能处理光点出生/消失；不能当成普通空帧提前退出。
    frames[0].num_elements=0;frames[1].num_elements=0;
    const auto* born=LookupPair(&animation,1,frames[0],frames[1],frames[2]);
    assert(born->trajectory && born->matrices.size()==3 && born->tweened==3);
    const auto* dying=LookupPair(&animation,2,frames[1],frames[2],frames[0]);
    assert(dying->trajectory && dying->matrices.size()==3 && dying->tweened==3);
    forcedOriginal=nullptr;
    InterlockedExchange(&g_skeletalTweenEnabled,0);InvalidatePlans();
}

// 生成小型x64调用夹具，检验三路rbx上下文传递；Main包装器再读取父实体。
uintptr_t captured = 0;
const void* Capture(const void*,int32_t,float,const void* key) {
    captured=reinterpret_cast<uintptr_t>(key);
    return key;
}
void CheckPublish() {
    auto* code=static_cast<uint8_t*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));
    assert(code);
    uintptr_t context[2]={0x1234,0x5678};
    std::array<Hook,4> hooks{};
    for (size_t i=0;i<4;++i) {
        auto* fn=code+i*64;
        // push rbx; sub rsp,32; mov rbx,imm64; call rel32; add rsp,32; pop rbx; ret
        const uint8_t body[]={0x53,0x48,0x83,0xec,0x20,0x48,0xbb,0,0,0,0,0,0,0,0,0xe8,0,0,0,0,0x48,0x83,0xc4,0x20,0x5b,0xc3};
        memcpy(fn,body,sizeof(body));
        const uintptr_t pointer=reinterpret_cast<uintptr_t>(context);
        memcpy(fn+7,&pointer,8);
        fn[48]=0xc3;
        const int32_t oldDelta=static_cast<int32_t>((fn+48)-(fn+20));
        memcpy(fn+16,&oldDelta,4);
        hooks[i].call=fn+15; hooks[i].before=oldDelta;
        hooks[i].replacement=reinterpret_cast<const void*>(Capture); hooks[i].parent=false;
        assert(PrepareStub(hooks[i]));
    }
    // 新增的第四个调用点不符时，其他位移也不能提前发布。
    hooks[3].call[1] ^= 1;
    assert(!Publish(hooks));
    assert(!memcmp(hooks[0].call + 1, &hooks[0].before, 4));
    hooks[3].call[1] ^= 1;
    FrameIdentityHook identity;
    auto* identityCaller = code + 256;
    // sub rsp,6D8h; mov [rsp+6C8h],rcx; <原始 mov>; mov rax,rdx; add rsp,6D8h; ret
    const uint8_t prologue[] = {0x48,0x81,0xEC,0xD8,0x06,0,0,0x48,0x89,0x8C,0x24,0xC8,0x06,0,0};
    memcpy(identityCaller, prologue, sizeof(prologue));
    identity.site = identityCaller + sizeof(prologue);
    memcpy(identity.site, identity.before.data(), identity.before.size());
    const uint8_t epilogue[] = {0x48,0x89,0xD0,0x48,0x81,0xC4,0xD8,0x06,0,0,0xC3};
    memcpy(identity.site + 8, epilogue, sizeof(epilogue));
    assert(PrepareFrameIdentityStub(identity));
    // 五处修改必须统一拒绝：任一目标不符时不能发布其他钩子。
    identity.site[0] ^= 1;
    assert(!Publish(hooks, &identity));
    assert(!memcmp(hooks[0].call + 1, &hooks[0].before, 4));
    identity.site[0] ^= 1;
    assert(Publish(hooks, &identity));
    for (size_t i=0;i<4;++i) {
        captured=0;
        auto call=reinterpret_cast<const void*(*)(const void*,int32_t,float)>(code+i*64);
        call(nullptr,0,0);
        assert(captured==reinterpret_cast<uintptr_t>(context));
    }
    auto identityCall = reinterpret_cast<const AnimFrame*(*)(const AnimFrame*)>(identityCaller);
    AnimFrame source{};
    auto& slot = *g_tlsAnimState->slots[0];
    slot.original = &source;
    assert(identityCall(&slot.frame) == &source);
    assert(identityCall(&source) == &source);
    // 使用 Windows 的展开器验证动态 relay 的真实栈布局。
    alignas(16) uint64_t stack[32]{};
    stack[25] = 0x12345678;
    CONTEXT unwindContext{};
    unwindContext.Rsp = reinterpret_cast<DWORD64>(stack);
    unwindContext.Rip = reinterpret_cast<DWORD64>(identity.stub + 8);
    DWORD64 imageBase = 0;
    auto* runtime = RtlLookupFunctionEntry(unwindContext.Rip, &imageBase, nullptr);
    assert(runtime == identity.unwind);
    PVOID handlerData = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, unwindContext.Rip, runtime,
        &unwindContext, &handlerData, &establisher, nullptr);
    assert(unwindContext.Rip == 0x12345678 && unwindContext.Rsp == reinterpret_cast<DWORD64>(stack + 26));
    assert(!Publish(hooks, &identity)); // 已修改的调用点不能再次覆盖。
    identity.Release();
    for(auto& hook:hooks) hook.Release();
    VirtualFree(code,0,MEM_RELEASE);
}

void CheckIdentityRegisters() {
    // 故意破坏所有易失寄存器，确认 relay 的保存/恢复确实生效。
    auto* memory = static_cast<uint8_t*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));
    assert(memory);
    std::vector<uint8_t> code;
    auto emit = [&](std::initializer_list<uint8_t> bytes) { code.insert(code.end(),bytes); };
    auto imm64 = [&](uint64_t value) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
        code.insert(code.end(),bytes,bytes+8);
    };
    struct Snapshot { uint64_t gpr[6]; uint8_t xmm[96]; const AnimFrame* frame; } snapshot{};
    AnimFrame source{}, synthetic{};
    snapshot.frame = &synthetic;
    for (unsigned i=0;i<96;++i) snapshot.xmm[i]=static_cast<uint8_t>(i+1);
    uint64_t capture[2]{};
    // 独立 leaf 夹具记录输入和对齐，再破坏易失寄存器。
    emit({0x48,0xB8}); imm64(reinterpret_cast<uint64_t>(capture));
    emit({0x48,0x89,0x08,0x48,0x89,0xE1,0x48,0x83,0xE1,0x0F,0x48,0x89,0x48,0x08});
    emit({0x48,0x31,0xC9});
    for(uint8_t reg=0;reg<4;++reg) emit({0x4D,0x31,static_cast<uint8_t>(0xC0+reg*9)});
    for(uint8_t reg=0;reg<6;++reg) emit({0x66,0x0F,0xEF,static_cast<uint8_t>(0xC0+reg*9)});
    emit({0x48,0xB8}); imm64(reinterpret_cast<uint64_t>(&source)); emit({0xC3});
    memcpy(memory+1024,code.data(),code.size());
    code.clear();
    // RBX 是非易失寄存器；压栈后保留 6D0h 字节，调用前仍为 16 字节对齐。
    emit({0x53,0x48,0x81,0xEC,0xD0,0x06,0,0,0x48,0x89,0xCB});
    emit({0x48,0x8B,0x83,0x90,0,0,0,0x48,0x89,0x84,0x24,0xC8,0x06,0,0});
    const uint8_t registers[] = {0,1,8,9,10,11};
    for(unsigned i=0;i<6;++i) {
        emit({static_cast<uint8_t>(registers[i]>=8?0x49:0x48),static_cast<uint8_t>(0xB8+(registers[i]&7))});
        imm64(0x12340000+i);
        emit({0xF3,0x0F,0x6F,static_cast<uint8_t>(0x83+i*8),static_cast<uint8_t>(48+i*16),0,0,0});
    }
    FrameIdentityHook identity;
    identity.site=memory+code.size();
    code.insert(code.end(),identity.before.begin(),identity.before.end());
    for(unsigned i=0;i<6;++i) {
        emit({static_cast<uint8_t>(registers[i]>=8?0x4C:0x48),0x89,
            static_cast<uint8_t>(0x43+(registers[i]&7)*8),static_cast<uint8_t>(i*8)});
        emit({0xF3,0x0F,0x7F,static_cast<uint8_t>(0x83+i*8),static_cast<uint8_t>(48+i*16),0,0,0});
    }
    emit({0x48,0x89,0xD0,0x48,0x81,0xC4,0xD0,0x06,0,0,0x5B,0xC3});
    memcpy(memory,code.data(),code.size());
    assert(PrepareFrameIdentityStub(identity));
    DWORD old=0;
    assert(VirtualProtect(identity.stub,512,PAGE_READWRITE,&old));
    const uintptr_t original=reinterpret_cast<uintptr_t>(&ResolveFrameIdentity);
    const uintptr_t clobber=reinterpret_cast<uintptr_t>(memory+1024);
    unsigned replaced=0;
    for(size_t i=0;i+8<identity.unwind->EndAddress;++i) {
        if (!memcmp(identity.stub+i,&original,8)) { memcpy(identity.stub+i,&clobber,8); ++replaced; }
    }
    assert(replaced==1);
    assert(VirtualProtect(identity.stub,512,PAGE_EXECUTE_READ,&old));
    assert(FlushInstructionCache(GetCurrentProcess(),identity.stub,512));
    memcpy(identity.site,identity.after.data(),identity.after.size());
    assert(FlushInstructionCache(GetCurrentProcess(),memory,4096));
    auto call=reinterpret_cast<const AnimFrame*(*)(Snapshot*)>(memory);
    assert(call(&snapshot)==&source);
    assert(capture[0]==reinterpret_cast<uint64_t>(&synthetic) && capture[1]==8);
    for(unsigned i=0;i<6;++i) assert(snapshot.gpr[i]==0x12340000+i);
    for(unsigned i=0;i<96;++i) assert(snapshot.xmm[i]==i+1);
    identity.Release();
    VirtualFree(memory,0,MEM_RELEASE);
}

void CheckLogRotation() {
    assert(IsSessionLog(L"animation_tween-20261122-0642.log"));
    assert(IsSessionLog(L"animation_tween-20261122-064200-000-123.log"));
    assert(!IsSessionLog(L"animation_tween-20261122-064.log"));
    assert(!IsSessionLog(L"animation_tween-20261122-06x2.log"));
    wchar_t executable[32768]{};
    assert(GetModuleFileNameW(nullptr, executable, 32768));
    std::wstring root(executable);
    root.resize(root.find_last_of(L"\\/"));
    root += L"\\log-rotation-" + std::to_wstring(GetCurrentProcessId());
    assert(CreateDirectoryW(root.c_str(), nullptr));
    auto touch = [&](const std::wstring& name) {
        FILE* file = nullptr;
        assert(!_wfopen_s(&file, (root + L"\\" + name).c_str(), L"wb") && file);
        fputs("keep\n", file);
        fclose(file);
    };
    for (unsigned i = 0; i < 15; ++i) {
        wchar_t name[128]{};
        if (i % 2) swprintf_s(name, L"animation_tween-20000101-00%02u.log", i);
        else swprintf_s(name, L"animation_tween-20000101-000000-000-%02u.log", i);
        touch(name);
    }
    touch(L"animation_tween.log");
    touch(L"unrelated.log");
    touch(L"animation_tween-not-a-session.log");
    // 日志初始化失败不能清理旧记录或设置成功标志。
    assert(!PrepareSessionLog(root + L"\\unrelated.log"));
    assert(g_logPath.empty());
    assert(PrepareSessionLog(root));
    const auto session = g_logPath;
    assert(session.substr(session.find_last_of(L"\\/") + 1).size() == 33);
    Log("first_marker");
    assert(PrepareLog() && g_logPath == session);
    assert(PrepareSessionLog(root) && g_logPath == session);
    Log("second_marker");
    FILE* file = nullptr;
    assert(!_wfopen_s(&file, session.c_str(), L"rb") && file);
    char contents[512]{};
    const auto length = fread(contents, 1, sizeof(contents) - 1, file);
    fclose(file);
    assert(length && strstr(contents, "first_marker") && strstr(contents, "second_marker"));
    assert(GetFileAttributesW((root + L"\\animation_tween.log").c_str()) == INVALID_FILE_ATTRIBUTES);
    unsigned owned = 0, unrelated = 0;
    WIN32_FIND_DATAW item{};
    HANDLE search = FindFirstFileW((root + L"\\*").c_str(), &item);
    assert(search != INVALID_HANDLE_VALUE);
    do {
        if (item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (IsSessionLog(item.cFileName)) ++owned;
        else ++unrelated;
        assert(DeleteFileW((root + L"\\" + item.cFileName).c_str()));
    } while (FindNextFileW(search, &item));
    FindClose(search);
    assert(owned == 10 && unrelated == 2);
    assert(RemoveDirectoryW(root.c_str()));
    g_logPath.clear();
}

static uintptr_t expectedOverlayPath = 0;
static unsigned overlayRenderDepth = 0;
static void CaptureOverlayRender(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7, uintptr_t a8, uintptr_t a9, uintptr_t a10, uintptr_t a11, uintptr_t a12, uintptr_t a13, uintptr_t a14, uintptr_t a15, uintptr_t a16, uintptr_t a17, uintptr_t a18, uintptr_t a19, uintptr_t a20, uintptr_t a21, uintptr_t a22, uintptr_t a23, uintptr_t a24, uintptr_t a25, uintptr_t a26) {
    const uintptr_t values[] = {a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20, a21, a22, a23, a24, a25, a26};
    for (unsigned i=0;i<26;++i) assert(values[i] == i+1);
    assert(g_tlsLastParentKey == expectedOverlayPath && g_tlsOverlayChildKey == 0);
    if (!overlayRenderDepth++) {
        const auto parent = g_tlsLastParentKey;
        g_tlsOverlayChildKey = 0xDEF;
        expectedOverlayPath = 0xDEF;
        RenderOverlayWithPath(1ULL, 2ULL, 3ULL, 4ULL, 5ULL, 6ULL, 7ULL, 8ULL, 9ULL, 10ULL, 11ULL, 12ULL, 13ULL, 14ULL, 15ULL, 16ULL, 17ULL, 18ULL, 19ULL, 20ULL, 21ULL, 22ULL, 23ULL, 24ULL, 25ULL, 26ULL);
        assert(g_tlsLastParentKey == parent && g_tlsOverlayChildKey == 0xDEF);
    }
}
static uintptr_t capturedItem = 0, capturedNode = 0;
static const void* CaptureOverlayDraw(const void* anim, int mode, float time, const OverlayDrawContext* draw) {
    assert(reinterpret_cast<uintptr_t>(anim) == 0x1234 && mode == 7 && time == 1.25f);
    capturedItem = reinterpret_cast<uintptr_t>(draw->item);
    capturedNode = reinterpret_cast<uintptr_t>(draw->node);
    return draw->item;
}
void CheckOverlayPath() {
    std::array<uint8_t, 0x240> first{}, moved{};
    std::array<uint8_t, 0x40> node{}, movedNode{};
    auto setup = [](auto& n, auto& items, uint32_t symbol) {
        const uintptr_t begin = reinterpret_cast<uintptr_t>(items.data()), end = begin + items.size();
        memcpy(n.data()+0x18,&symbol,4); memcpy(n.data()+0x30,&begin,8); memcpy(n.data()+0x38,&end,8);
    };
    setup(node, first, 17); setup(movedNode, moved, 17);
    int animation = 0;
    g_tlsLastParentKey = 0x123456;
    const auto firstKey = OverlayTrackKey(&animation,{first.data(),node.data()});
    assert(firstKey && firstKey == OverlayTrackKey(&animation,{moved.data(),movedNode.data()}));
    assert(firstKey != OverlayTrackKey(&animation,{moved.data()+0x120,movedNode.data()}));
    setup(movedNode,moved,18);
    assert(firstKey != OverlayTrackKey(&animation,{moved.data(),movedNode.data()}));
    setup(movedNode,moved,17); g_tlsLastParentKey = 0x234567;
    assert(firstKey != OverlayTrackKey(&animation,{moved.data(),movedNode.data()}));
    assert(!OverlayTrackKey(&animation,{moved.data()+1,movedNode.data()}));
    assert(!OverlayTrackKey(&animation,{moved.data()+moved.size(),movedNode.data()}));
    // 26个参数逐项核对，并验证嵌套返回后恢复父路径。
    g_origOverlayRender = CaptureOverlayRender;
    g_tlsLastParentKey = 0xABC; g_tlsOverlayChildKey = expectedOverlayPath = 0xCDE;
    Hook recursive{}; recursive.call = reinterpret_cast<uint8_t*>(&CaptureOverlayRender);
    recursive.replacement = reinterpret_cast<const void*>(&RenderOverlayWithPath); recursive.direct = true;
    assert(PrepareStub(recursive));
    reinterpret_cast<OverlayRender_t>(recursive.stub)(1ULL, 2ULL, 3ULL, 4ULL, 5ULL, 6ULL, 7ULL, 8ULL, 9ULL, 10ULL, 11ULL, 12ULL, 13ULL, 14ULL, 15ULL, 16ULL, 17ULL, 18ULL, 19ULL, 20ULL, 21ULL, 22ULL, 23ULL, 24ULL, 25ULL, 26ULL);
    assert(overlayRenderDepth == 2 && g_tlsLastParentKey == 0xABC && g_tlsOverlayChildKey == 0xCDE);
    recursive.Release();
    // 实际机器码relay应传递rbx+rdi，保留原来三个取帧参数。
    auto* code = static_cast<uint8_t*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));
    assert(code);
    const uint8_t body[] = {0x53,0x57,0x48,0x83,0xEC,0x28,0x48,0xBB,0,0,0,0,0,0,0,0,
        0x48,0xBF,0,0,0,0,0,0,0,0,0xE8,0,0,0,0,0x48,0x83,0xC4,0x28,0x5F,0x5B,0xC3};
    memcpy(code,body,sizeof(body)); const uintptr_t item=0x3456, map=0x789A;
    memcpy(code+8,&item,8); memcpy(code+18,&map,8);
    Hook overlay{}; overlay.call=code+26; overlay.overlay=true;
    overlay.replacement=reinterpret_cast<const void*>(&CaptureOverlayDraw);
    assert(PrepareStub(overlay)); memcpy(overlay.call+1,&overlay.after,4);
    auto call=reinterpret_cast<const void*(*)(const void*,int,float)>(code);
    assert(call(reinterpret_cast<const void*>(0x1234),7,1.25f)==reinterpret_cast<const void*>(item));
    assert(capturedItem==item && capturedNode==map);
    alignas(16) uintptr_t stack[16]{}; stack[7]=0x12345678;
    CONTEXT context{}; context.Rsp=reinterpret_cast<DWORD64>(stack); context.Rip=reinterpret_cast<DWORD64>(overlay.stub+19);
    DWORD64 image=0; auto* fn=RtlLookupFunctionEntry(context.Rip,&image,nullptr); assert(fn==overlay.unwind);
    PVOID data=nullptr; DWORD64 frame=0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER,image,context.Rip,fn,&context,&data,&frame,nullptr);
    assert(context.Rip==0x12345678 && context.Rsp==reinterpret_cast<DWORD64>(stack+8));
    overlay.Release(); VirtualFree(code,0,MEM_RELEASE);
    g_origOverlayRender=nullptr; g_tlsLastParentKey=0; g_tlsOverlayChildKey=0;
}

int main() {
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    CheckMatrixAxes();
    CheckLogRotation();
    CheckCore();
    CheckUnsupportedFireflyFallback();
    CheckRepeatedRegularBones();
    CheckOrdinaryCorrespondence();
    CheckEndpointTransitions();
    CheckFrameGeometry();
    CheckNestedFrames();
    CheckOverlayPath();
    CheckMenuBatGroups();
    CheckLocalPlans();
    CheckFireflyInterpolation();
    CheckPublish();
    CheckIdentityRegisters();
    uint64_t stats[5]{};
    assert(HS_ReadStats(stats,5)==1 && stats[0]==0 && stats[1]==0);
    assert(HS_ReadStats(stats,4)==0);
    assert(HS_Initialize()==0 && !strcmp(HS_LastError(),"unsupported_executable"));
    puts("PASS: stable overlay attachment paths across relocated snapshots, distinct symbols/indices/parents, nested restoration, 26 ABI arguments, overlay context relay/unwind, five-patch transaction, affine row axes/nonuniform scale/reflection/angle wrap/zero scale/real client ray monotonicity, frame geometry endpoint/order/meta/signature gates, ten retained nested frames and distinct overlay instances, conservative reciprocal correspondence/layer transitions/ambiguous holds/endpoint births and deaths, menu bat group identity/coherent guards/births, log rotation, interpolation, cached plans, frame identity, source immutability, publication, relays, unwind, stats and executable rejection");
    return 0;
}
