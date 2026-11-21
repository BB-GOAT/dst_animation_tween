// 缓存仅保存自有输入快照、对应索引和矩阵常量，不拥有任何引擎资源。
// 同一动画地址被释放后复用、或元素原地改变，都按实际内容重新计算。
struct FrameSnapshot {
    std::vector<AnimElement> elements;

    void Capture(const AnimFrame& frame) {
        if (frame.num_elements) elements.assign(frame.elements, frame.elements + frame.num_elements);
    }
    bool Matches(const AnimFrame& frame) const noexcept {
        return frame.num_elements == elements.size() &&
               (elements.empty() ||
                (frame.elements && !memcmp(frame.elements, elements.data(), elements.size() * sizeof(AnimElement))));
    }
};

struct ElementSource {
    uint32_t index;
    bool target;
};

struct PairPlan {
    FrameSnapshot first, second, previous;
    std::vector<MatrixPlan> matrices;
    std::vector<uint32_t> targetSprite;
    std::vector<uint8_t> swaps;
    std::vector<ElementSource> sources;
    uint32_t tweened = 0, pass2 = 0;
    bool needsHistory = false;
    bool trajectory = false;
    bool bypass = false;

    bool Matches(const AnimFrame& prev, const AnimFrame& f0, const AnimFrame& f1) const noexcept {
        return first.Matches(f0) && second.Matches(f1) && (!needsHistory || previous.Matches(prev));
    }
    size_t Bytes() const noexcept {
        return sizeof(*this) +
               (first.elements.capacity() + second.elements.capacity() + previous.elements.capacity()) *
                   sizeof(AnimElement) +
               matrices.capacity() * sizeof(MatrixPlan) + targetSprite.capacity() * sizeof(uint32_t) +
               swaps.capacity() + sources.capacity() * sizeof(ElementSource);
    }
};

constexpr uint32_t kFireflySymbol = 979885181u;
constexpr uint32_t kFireflyLayer = 434069731u;
constexpr uint32_t kTrajectoryLimit = 7;

static bool IsFireflyElement(const AnimElement& element) noexcept {
    return element.layer_hash == kFireflyLayer && element.symbol_hash == kFireflySymbol;
}
static bool ContainsFirefly(const AnimFrame& frame) noexcept {
    for (uint32_t i = 0; i < frame.num_elements; ++i)
        if (IsFireflyElement(frame.elements[i])) return true;
    return false;
}
static bool HasRepeatedFireflyElements(const AnimFrame& frame) noexcept {
    uint32_t count = 0;
    for (uint32_t i = 0; i < frame.num_elements; ++i) {
        if (IsFireflyElement(frame.elements[i]) && ++count == 2) return true;
    }
    return false;
}
static bool IsTrajectoryFrame(const AnimFrame& frame) noexcept {
    if (frame.num_elements > kTrajectoryLimit || (frame.num_elements && !frame.elements)) return false;
    for (uint32_t i = 0; i < frame.num_elements; ++i) {
        if (!IsFireflyElement(frame.elements[i])) return false;
        // 局部预测的证据只覆盖正常有限坐标，不对无效资源猜测对应。
        for (uint16_t half : frame.elements[i].m)
            if ((half & 0x7C00u) == 0x7C00u) return false;
    }
    return true;
}

using ElementVelocity = std::array<double, 2>;

// 有界的一对一动态规划，仅用于已经核实、每帧至多七个元素的光点组。
// 出生/消失各罚400像素平方；它是运动连续性假设，不是原始作者实例ID。
static std::vector<int> SolveTrajectory(const AnimFrame& first, const AnimFrame& second,
                                        const std::vector<ElementVelocity>& velocity) {
    const uint32_t n0 = first.num_elements, n1 = second.num_elements;
    const uint32_t masks = 1u << n1;
    std::vector<double> costs((n0 + 1) * masks, 0.0);
    std::vector<int> choices(n0 * masks, -1);
    for (uint32_t mask = 0; mask < masks; ++mask) {
        uint32_t used = 0;
        for (uint32_t bits = mask; bits; bits &= bits - 1) ++used;
        costs[n0 * masks + mask] = 400.0 * (n1 - used);
    }
    for (uint32_t i = n0; i-- > 0;)
        for (uint32_t mask = 0; mask < masks; ++mask) {
            double best = 400.0 + costs[(i + 1) * masks + mask];
            int choice = -1;
            const double px = HalfToFloat(first.elements[i].m[4]) + velocity[i][0];
            const double py = HalfToFloat(first.elements[i].m[5]) + velocity[i][1];
            for (uint32_t j = 0; j < n1; ++j)
                if (!(mask & (1u << j))) {
                    const double dx = px - HalfToFloat(second.elements[j].m[4]);
                    const double dy = py - HalfToFloat(second.elements[j].m[5]);
                    const double cost = costs[(i + 1) * masks + (mask | (1u << j))] + dx * dx + dy * dy;
                    if (cost < best) {
                        best = cost;
                        choice = static_cast<int>(j);
                    }
                }
            costs[i * masks + mask] = best;
            choices[i * masks + mask] = choice;
        }
    std::vector<int> mapping(n0, -1);
    uint32_t mask = 0;
    for (uint32_t i = 0; i < n0; ++i) {
        const int j = choices[i * masks + mask];
        mapping[i] = j;
        if (j >= 0) mask |= 1u << j;
    }
    return mapping;
}

static std::vector<int> BuildLocalCorrespondence(const AnimFrame& previous, const AnimFrame& first,
                                                 const AnimFrame& second) {
    const auto previousMap =
        SolveTrajectory(previous, first, std::vector<ElementVelocity>(previous.num_elements, {0.0, 0.0}));
    std::vector<ElementVelocity> velocity(first.num_elements, {0.0, 0.0});
    for (uint32_t i = 0; i < previous.num_elements; ++i)
        if (previousMap[i] >= 0) {
            const auto j = static_cast<uint32_t>(previousMap[i]);
            velocity[j] = {
                static_cast<double>(HalfToFloat(first.elements[j].m[4]) - HalfToFloat(previous.elements[i].m[4])),
                static_cast<double>(HalfToFloat(first.elements[j].m[5]) - HalfToFloat(previous.elements[i].m[5]))};
        }
    return SolveTrajectory(first, second, velocity);
}

static std::unique_ptr<PairPlan> PreparePair(const AnimFrame& prev, const AnimFrame& f0, const AnimFrame& f1) {
    auto plan = std::make_unique<PairPlan>();
    plan->first.Capture(f0);
    plan->second.Capture(f1);
    plan->needsHistory = ContainsFirefly(f0) || ContainsFirefly(f1);
    if (plan->needsHistory) {
        if (prev.num_elements > 512 || (prev.num_elements && !prev.elements)) return nullptr;
        plan->previous.Capture(prev);
        plan->trajectory = IsTrajectoryFrame(prev) && IsTrajectoryFrame(f0) && IsTrajectoryFrame(f1);
    }
    plan->bypass = !plan->trajectory && (HasRepeatedFireflyElements(f0) || HasRepeatedFireflyElements(f1));
    if (plan->bypass || (!plan->trajectory && (!f0.num_elements || !f1.num_elements))) return plan;
    const uint32_t n0 = f0.num_elements, n1 = f1.num_elements;
    std::vector<int> mapping(n0, -1);
    std::vector<uint8_t> swaps(n0, 0), used(n1, 0);
    if (plan->trajectory) mapping = BuildLocalCorrespondence(prev, f0, f1);
    else
        for (uint32_t i = 0; i < n0; ++i) {
            // 普通动画保持0.1.3的对应关系，仅将计算移至缓存建立时。
            const auto& e = f0.elements[i];
            uint32_t occurrence = 0;
            for (uint32_t k = 0; k < i; ++k) {
                const auto& q = f0.elements[k];
                if (q.layer_hash == e.layer_hash && q.symbol_hash == e.symbol_hash && q.frame_num == e.frame_num)
                    ++occurrence;
            }
            uint32_t seen = 0;
            for (uint32_t j = 0; j < n1; ++j) {
                const auto& q = f1.elements[j];
                if (q.layer_hash == e.layer_hash && q.symbol_hash == e.symbol_hash && q.frame_num == e.frame_num &&
                    seen++ == occurrence) {
                    mapping[i] = static_cast<int>(j);
                    break;
                }
            }
            if (mapping[i] >= 0) continue;
            occurrence = 0;
            seen = 0;
            for (uint32_t k = 0; k < i; ++k) {
                const auto& q = f0.elements[k];
                if (q.layer_hash == e.layer_hash && q.symbol_hash == e.symbol_hash) ++occurrence;
            }
            for (uint32_t j = 0; j < n1; ++j) {
                const auto& q = f1.elements[j];
                if (q.layer_hash == e.layer_hash && q.symbol_hash == e.symbol_hash && seen++ == occurrence) {
                    mapping[i] = static_cast<int>(j);
                    swaps[i] = 1;
                    break;
                }
            }
        }
    for (uint32_t i = 0; i < n0; ++i)
        if (mapping[i] >= 0) used[mapping[i]] = 1;
    const auto append = [&](uint32_t index, bool target) {
        const auto& start = target ? f1.elements[index] : f0.elements[index];
        const int match = target ? static_cast<int>(index) : mapping[index];
        MatrixPlan matrix;
        bool swap = false;
        uint32_t sprite = start.frame_num;
        if (target || (plan->trajectory && match < 0)) {
            // 资产中的光点以近零缩放出现/消失；缺失一端在原位置缩至零。
            auto zero = start;
            zero.m[0] = zero.m[1] = zero.m[2] = zero.m[3] = 0;
            matrix = target ? PrepareMatrix(zero.m, start.m) : PrepareMatrix(start.m, zero.m);
        } else if (match >= 0) {
            const auto& end = f1.elements[match];
            matrix = PrepareMatrix(start.m, end.m);
            swap = plan->trajectory ? start.frame_num != end.frame_num : swaps[index] != 0;
            sprite = end.frame_num;
        }
        plan->sources.push_back({index, target});
        plan->matrices.push_back(matrix);
        plan->targetSprite.push_back(sprite);
        plan->swaps.push_back(swap ? 1 : 0);
        if (matrix.mode) {
            ++plan->tweened;
            if (swap) ++plan->pass2;
        }
    };
    uint32_t nextBirth = 0;
    for (uint32_t i = 0; i < n0; ++i) {
        // 已核实的光点对应保留次序；将出生点插入其目标层次，而非一律追加。
        if (plan->trajectory && mapping[i] >= 0) {
            while (nextBirth < static_cast<uint32_t>(mapping[i])) {
                if (!used[nextBirth]) append(nextBirth, true);
                ++nextBirth;
            }
        }
        append(i, false);
    }
    if (plan->trajectory)
        for (; nextBirth < n1; ++nextBirth)
            if (!used[nextBirth]) append(nextBirth, true);
    return plan;
}

struct PairKey {
    const Animation* animation;
    uint32_t frame;
    bool operator==(const PairKey& other) const noexcept {
        return animation == other.animation && frame == other.frame;
    }
};
struct PairHash {
    size_t operator()(const PairKey& key) const noexcept {
        return (reinterpret_cast<uintptr_t>(key.animation) >> 4) ^ (static_cast<size_t>(key.frame) * 0x9E3779B9u);
    }
};
volatile LONG64 g_planEpoch = 1;
static void InvalidatePlans() noexcept { InterlockedIncrement64(&g_planEpoch); }

struct PairCache {
    struct Entry {
        std::unique_ptr<PairPlan> plan;
        std::list<PairKey>::iterator age;
    };
    std::unordered_map<PairKey, Entry, PairHash> entries;
    std::list<PairKey> order;
    PairKey lastKey{};
    PairPlan* last = nullptr;
    LONG64 epoch = 0;
    size_t bytes = 0;
    uint64_t builds = 0;
    void Clear() noexcept {
        last = nullptr;
        entries.clear();
        order.clear();
        bytes = 0;
    }
};
thread_local PairCache g_pairCache;
// 每个绘制线程的计划内容预算；容器节点、桶和建立新计划时的短暂峰值另计。
constexpr size_t kPlanBudget = 16 * 1024 * 1024;
constexpr size_t kPlanEntryLimit = 4096;

static const PairPlan* LookupPair(const Animation* animation, uint32_t frame, const AnimFrame& previous,
                                  const AnimFrame& f0, const AnimFrame& f1,
                                  LONG64 epoch = InterlockedCompareExchange64(&g_planEpoch, 0, 0)) {
    auto& cache = g_pairCache;
    if (cache.epoch != epoch) {
        cache.Clear();
        cache.epoch = epoch;
    }
    const PairKey key{animation, frame};
    if (cache.last && cache.lastKey == key && cache.last->Matches(previous, f0, f1)) return cache.last;
    auto found = cache.entries.find(key);
    if (found != cache.entries.end() && found->second.plan->Matches(previous, f0, f1)) {
        cache.lastKey = key;
        cache.last = found->second.plan.get();
        return cache.last;
    }
    auto plan = PreparePair(previous, f0, f1);
    if (!plan) return nullptr;
    ++cache.builds;
    const size_t bytes = plan->Bytes();
    if (found != cache.entries.end()) {
        cache.bytes -= found->second.plan->Bytes();
        cache.last = nullptr;
        cache.order.erase(found->second.age);
        cache.entries.erase(found);
    }
    // 预算仅控制保留多少计划；淘汰后仍使用同一算法正常重建。
    // 按写入次序逐项淘汰，命中路径不维护链表，也不因一项超限清空全部工作集。
    while (!cache.order.empty() && (cache.bytes + bytes > kPlanBudget || cache.entries.size() >= kPlanEntryLimit)) {
        const auto oldest = cache.entries.find(cache.order.front());
        cache.bytes -= oldest->second.plan->Bytes();
        cache.last = nullptr;
        cache.entries.erase(oldest);
        cache.order.pop_front();
    }
    cache.order.push_back(key);
    const auto age = std::prev(cache.order.end());
    try {
        cache.entries.emplace(key, PairCache::Entry{std::move(plan), age});
    } catch (...) {
        cache.order.pop_back();
        throw;
    }
    cache.bytes += bytes;
    cache.lastKey = key;
    cache.last = cache.entries.find(key)->second.plan.get();
    return cache.last;
}
