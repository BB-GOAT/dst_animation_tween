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
    // 两个端点各自的几何槽位到插值计划的对应；不假定目标次序等于源次序。
    std::vector<int> firstOrder, secondOrder;
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
               swaps.capacity() + sources.capacity() * sizeof(ElementSource) +
               (firstOrder.capacity() + secondOrder.capacity()) * sizeof(int);
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
                                        const std::vector<ElementVelocity>& velocity, double unmatchedCost = 400.0) {
    const uint32_t n0 = first.num_elements, n1 = second.num_elements;
    const uint32_t masks = 1u << n1;
    std::vector<double> costs((n0 + 1) * masks, 0.0);
    std::vector<int> choices(n0 * masks, -1);
    for (uint32_t mask = 0; mask < masks; ++mask) {
        uint32_t used = 0;
        for (uint32_t bits = mask; bits; bits &= bits - 1) ++used;
        costs[n0 * masks + mask] = unmatchedCost * (n1 - used);
    }
    for (uint32_t i = n0; i-- > 0;)
        for (uint32_t mask = 0; mask < masks; ++mask) {
            double best = unmatchedCost + costs[(i + 1) * masks + mask];
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
                                                 const AnimFrame& second, double unmatchedCost = 400.0) {
    const auto previousMap =
        SolveTrajectory(previous, first, std::vector<ElementVelocity>(previous.num_elements, {0.0, 0.0}), unmatchedCost);
    std::vector<ElementVelocity> velocity(first.num_elements, {0.0, 0.0});
    for (uint32_t i = 0; i < previous.num_elements; ++i)
        if (previousMap[i] >= 0) {
            const auto j = static_cast<uint32_t>(previousMap[i]);
            velocity[j] = {
                static_cast<double>(HalfToFloat(first.elements[j].m[4]) - HalfToFloat(previous.elements[i].m[4])),
                static_cast<double>(HalfToFloat(first.elements[j].m[5]) - HalfToFloat(previous.elements[i].m[5]))};
        }
    return SolveTrajectory(first, second, velocity, unmatchedCost);
}

// 已核实的菜单蝙蝠以本体、左翼、右翼连续存放；贴图片段不是实例身份。
constexpr uint32_t kMenuBatLayer = 2269683757u;
constexpr uint32_t kMenuBatBody = 3736508075u;
constexpr uint32_t kMenuBatWing = 1533683372u;
static bool IsMenuBatPart(const AnimElement& e) noexcept {
    return e.layer_hash == kMenuBatLayer && (e.symbol_hash == kMenuBatBody || e.symbol_hash == kMenuBatWing);
}
static bool ContainsMenuBat(const AnimFrame& frame) noexcept {
    for (uint32_t i = 0; i < frame.num_elements; ++i)
        if (IsMenuBatPart(frame.elements[i])) return true;
    return false;
}
struct MenuBatGroups {
    std::vector<uint32_t> indices;
    std::vector<AnimElement> bodies;
    bool Capture(const AnimFrame& frame) {
        if (frame.num_elements > 512 || (frame.num_elements && !frame.elements)) return false;
        for (uint32_t i = 0; i < frame.num_elements; ++i) {
            if (!IsMenuBatPart(frame.elements[i])) continue;
            if (indices.size() == kTrajectoryLimit || i + 2 >= frame.num_elements ||
                frame.elements[i].symbol_hash != kMenuBatBody) return false;
            for (uint32_t part = 0; part < 3; ++part) {
                const auto& e = frame.elements[i + part];
                if (e.layer_hash != kMenuBatLayer ||
                    e.symbol_hash != (part ? kMenuBatWing : kMenuBatBody)) return false;
                for (uint16_t half : e.m)
                    if ((half & 0x7C00u) == 0x7C00u) return false;
            }
            indices.push_back(i);
            bodies.push_back(frame.elements[i]);
            i += 2;
        }
        return true;
    }
    AnimFrame Frame() {
        AnimFrame frame{};
        frame.elements = bodies.data();
        frame.num_elements = static_cast<uint32_t>(bodies.size());
        return frame;
    }
};
static bool MapMenuBats(const AnimFrame& prev, const AnimFrame& f0, const AnimFrame& f1,
                        std::vector<int>& mapping, std::vector<uint8_t>& swaps,
                        std::vector<uint8_t>& fading, std::vector<uint8_t>& births,
                        std::vector<MatrixPlan>& coherent) {
    MenuBatGroups history, first, second;
    if (!history.Capture(prev) || !first.Capture(f0) || !second.Capture(f1)) return false;
    // 较萤火虫移动更快；只在计划建立时对最多七个本体求解，翅膀不独立抢目标。
    const auto groups = BuildLocalCorrespondence(history.Frame(), first.Frame(), second.Frame(), 1600.0);
    coherent.resize(f0.num_elements);
    std::vector<uint8_t> used(second.indices.size(), 0);
    for (uint32_t i = 0; i < first.indices.size(); ++i) {
        const int j = groups[i];
        if (j >= 0) used[j] = 1;
        for (uint32_t part = 0; part < 3; ++part) {
            const auto source = first.indices[i] + part;
            mapping[source] = j < 0 ? -1 : static_cast<int>(second.indices[j] + part);
            fading[source] = j < 0;
            swaps[source] = j >= 0 && f0.elements[source].frame_num != f1.elements[mapping[source]].frame_num;
        }
        if (j >= 0) {
            const auto source = first.indices[i], target = second.indices[j];
            bool safe = true;
            for (uint32_t part = 0; part < 3; ++part)
                if (!PrepareMatrix(f0.elements[source + part].m, f1.elements[target + part].m).mode) safe = false;
            if (!safe) {
                // 翅膀换片可能超过旋转护栏；不能单独冻结翅膀、让本体继续飞走。
                // 此区间保留整只姿态，共用本体的位移，下一原始帧再切换姿态。
                const float dx = HalfToFloat(f1.elements[target].m[4]) - HalfToFloat(f0.elements[source].m[4]);
                const float dy = HalfToFloat(f1.elements[target].m[5]) - HalfToFloat(f0.elements[source].m[5]);
                for (uint32_t part = 0; part < 3; ++part) {
                    auto& matrix = coherent[source + part];
                    matrix.mode = 1;
                    for (uint32_t k = 0; k < 6; ++k) matrix.v[k] = HalfToFloat(f0.elements[source + part].m[k]);
                    matrix.v[10] = dx;
                    matrix.v[11] = dy;
                }
            }
        }
    }
    for (uint32_t j = 0; j < second.indices.size(); ++j)
        if (!used[j])
            for (uint32_t part = 0; part < 3; ++part) births[second.indices[j] + part] = 1;
    return true;
}

// 重复元素的数组次序、贴图片段和图层都不是实例ID。
// 只在计划建立时比较完整变换；不能明确对应时保留原始端点，不把余项强行连接。
struct ContinuitySample {
    float m[6];
    float determinant;
};
static std::vector<int> BuildOrdinaryCorrespondence(const AnimFrame& first, const AnimFrame& second) {
    const uint32_t n0 = first.num_elements, n1 = second.num_elements;
    std::vector<int> mapping(n0, -1);
    std::vector<uint8_t> grouped(n0, 0), reserved(n1, 0);
    std::vector<ContinuitySample> start(n0), end(n1);
    const auto capture = [](const AnimFrame& frame, std::vector<ContinuitySample>& samples) {
        for (uint32_t i = 0; i < frame.num_elements; ++i) {
            for (uint32_t k = 0; k < 6; ++k) samples[i].m[k] = HalfToFloat(frame.elements[i].m[k]);
            const auto* m = samples[i].m;
            samples[i].determinant = m[0] * m[3] - m[1] * m[2];
        }
    };
    capture(first, start);
    capture(second, end);
    std::vector<uint32_t> sources, targets;
    for (uint32_t seed = 0; seed < n0; ++seed) {
        if (grouped[seed]) continue;
        const uint32_t symbol = first.elements[seed].symbol_hash;
        sources.clear(); targets.clear();
        for (uint32_t i = seed; i < n0; ++i) if (first.elements[i].symbol_hash == symbol) {
            grouped[i] = 1; sources.push_back(i);
        }
        for (uint32_t j = 0; j < n1; ++j)
            if (second.elements[j].symbol_hash == symbol) targets.push_back(j);
        if (targets.empty()) continue;
        if (sources.size() == 1 && targets.size() == 1 &&
            first.elements[sources[0]].layer_hash == second.elements[targets[0]].layer_hash) {
            mapping[sources[0]] = static_cast<int>(targets[0]); reserved[targets[0]] = 1;
            continue;
        }
        struct LayerPopulation { uint32_t layer, before = 0, after = 0; };
        std::vector<LayerPopulation> layers;
        const auto population = [&](uint32_t layer) -> LayerPopulation& {
            for (auto& item : layers) if (item.layer == layer) return item;
            layers.push_back({layer}); return layers.back();
        };
        for (uint32_t i : sources) ++population(first.elements[i].layer_hash).before;
        for (uint32_t j : targets) ++population(second.elements[j].layer_hash).after;
        std::vector<uint32_t> sourceOld(sources.size()), sourceNew(sources.size());
        std::vector<uint32_t> targetOld(targets.size()), targetNew(targets.size());
        for (size_t a = 0; a < sources.size(); ++a) {
            const auto& count = population(first.elements[sources[a]].layer_hash);
            sourceOld[a] = count.before; sourceNew[a] = count.after;
        }
        for (size_t b = 0; b < targets.size(); ++b) {
            const auto& count = population(second.elements[targets[b]].layer_hash);
            targetOld[b] = count.before; targetNew[b] = count.after;
        }
        // 同层同符号两端都只有一个元素时，保留普通骨骼的现有直接对应。
        for (size_t a = 0; a < sources.size(); ++a) if (sourceOld[a] == 1 && sourceNew[a] == 1) {
            for (size_t b = 0; b < targets.size(); ++b)
                if (first.elements[sources[a]].layer_hash == second.elements[targets[b]].layer_hash) {
                    mapping[sources[a]] = static_cast<int>(targets[b]); reserved[targets[b]] = 1; break;
                }
        }
        // 稳定、互异的片段集合仍保留原匹配方式，避免把衣服、头发等近距离合法部件
        // 仅因连续性分数接近而降为原始帧。重复片段、增删和片段分布变化不走此捷径。
        std::vector<uint8_t> stablePalette(sources.size(), 0), paletteChecked(sources.size(), 0);
        for (size_t a = 0; a < sources.size(); ++a) {
            if (paletteChecked[a] || mapping[sources[a]] >= 0 || sourceOld[a] != sourceNew[a]) continue;
            const uint32_t layer = first.elements[sources[a]].layer_hash;
            bool unique = true;
            for (uint32_t i : sources) if (first.elements[i].layer_hash == layer) {
                uint32_t before = 0, after = 0;
                for (uint32_t k : sources)
                    before += first.elements[k].layer_hash == layer && first.elements[k].frame_num == first.elements[i].frame_num;
                for (uint32_t j : targets)
                    after += second.elements[j].layer_hash == layer && second.elements[j].frame_num == first.elements[i].frame_num;
                if (before != 1 || after != 1) { unique = false; break; }
            }
            for (size_t k = a; k < sources.size(); ++k)
                if (first.elements[sources[k]].layer_hash == layer) {
                    stablePalette[k] = unique; paletteChecked[k] = 1;
                }
        }
        // 唯一且完全相同的姿态仍先于贴图片段，保留静止换片时的几何身份。
        for (size_t a = 0; a < sources.size(); ++a) if (stablePalette[a]) {
            const auto& x = first.elements[sources[a]];
            uint32_t before = 0, after = 0; int match = -1;
            for (uint32_t i : sources)
                before += first.elements[i].layer_hash == x.layer_hash && !memcmp(first.elements[i].m, x.m, sizeof(x.m));
            for (uint32_t j : targets)
                if (second.elements[j].layer_hash == x.layer_hash && !memcmp(second.elements[j].m, x.m, sizeof(x.m))) {
                    ++after; match = static_cast<int>(j);
                }
            if (before == 1 && after == 1 && !reserved[match]) {
                mapping[sources[a]] = match; reserved[match] = 1;
            }
        }
        for (size_t a = 0; a < sources.size(); ++a) if (stablePalette[a] && mapping[sources[a]] < 0) {
            const auto& x = first.elements[sources[a]];
            for (uint32_t j : targets)
                if (!reserved[j] && second.elements[j].layer_hash == x.layer_hash && second.elements[j].frame_num == x.frame_num) {
                    mapping[sources[a]] = static_cast<int>(j); reserved[j] = 1; break;
                }
        }
        constexpr float infinity = (std::numeric_limits<float>::max)();
        std::vector<float> rowBest(sources.size(), infinity), rowNext(sources.size(), infinity);
        std::vector<float> colBest(targets.size(), infinity), colNext(targets.size(), infinity);
        std::vector<int> rowTarget(sources.size(), -1), colSource(targets.size(), -1);
        const auto update = [](float cost, int index, float& best, float& next, int& chosen) {
            if (cost < best) { next = best; best = cost; chosen = index; }
            else if (cost < next) next = cost;
        };
        for (size_t a = 0; a < sources.size(); ++a) {
            if (mapping[sources[a]] >= 0) continue;
            for (size_t b = 0; b < targets.size(); ++b) {
                if (reserved[targets[b]]) continue;
                const auto& x = first.elements[sources[a]]; const auto& y = second.elements[targets[b]];
                // 跨层仅允许从数量减少的图层流向数量增加的图层，不任意重连稳定骨骼层。
                if (x.layer_hash != y.layer_hash &&
                    !(sourceOld[a] > sourceNew[a] && targetOld[b] < targetNew[b])) continue;
                const auto& u = start[sources[a]]; const auto& v = end[targets[b]];
                if (u.determinant * v.determinant < 0) continue;
                const float dx = u.m[4] - v.m[4], dy = u.m[5] - v.m[5];
                const float shift = dx * dx + dy * dy;
                // 可接受最优分数最多1600；位移分数已超过其四倍的候选，
                // 不会改变唯一最优和四倍间隔判据，提前跳过完整矩阵比较。
                if (!std::isfinite(shift) || shift > 6400.0f) continue;
                const __m128 delta = _mm_sub_ps(_mm_loadu_ps(u.m), _mm_loadu_ps(v.m));
                const __m128 square = _mm_mul_ps(delta, delta);
                const __m128 sum = _mm_add_ps(square, _mm_movehl_ps(square, square));
                const float shape = _mm_cvtss_f32(_mm_add_ss(sum, _mm_shuffle_ps(sum, sum, _MM_SHUFFLE(1,1,1,1))));
                const float cost = shape * 1024.0f + shift;
                if (!std::isfinite(cost)) continue;
                update(cost, static_cast<int>(b), rowBest[a], rowNext[a], rowTarget[a]);
                update(cost, static_cast<int>(a), colBest[b], colNext[b], colSource[b]);
            }
        }
        for (size_t a = 0; a < sources.size(); ++a) {
            const int b = rowTarget[a];
            // 40像素等效误差上限、双向唯一最优且与次选相隔四倍；这是保守连续性判据，
            // 不是恢复作者身份的证明。模糊、交叉、突变或超限者不强行补帧。
            if (b >= 0 && colSource[b] == static_cast<int>(a) && rowBest[a] <= 1600.0f &&
                rowBest[a] * 4.0f + 1e-6f < rowNext[a] && colBest[b] * 4.0f + 1e-6f < colNext[b])
                mapping[sources[a]] = static_cast<int>(targets[b]);
        }
    }
    return mapping;
}

static std::unique_ptr<PairPlan> PreparePair(const AnimFrame& prev, const AnimFrame& f0, const AnimFrame& f1) {
    auto plan = std::make_unique<PairPlan>();
    plan->first.Capture(f0);
    plan->second.Capture(f1);
    const bool menuBats = ContainsMenuBat(f0) || ContainsMenuBat(f1);
    plan->needsHistory = ContainsFirefly(f0) || ContainsFirefly(f1) || menuBats;
    if (plan->needsHistory) {
        if (prev.num_elements > 512 || (prev.num_elements && !prev.elements)) return nullptr;
        plan->previous.Capture(prev);
        plan->trajectory = IsTrajectoryFrame(prev) && IsTrajectoryFrame(f0) && IsTrajectoryFrame(f1);
    }
    plan->bypass = !plan->trajectory && (HasRepeatedFireflyElements(f0) || HasRepeatedFireflyElements(f1));
    if (plan->bypass || (!plan->trajectory && !menuBats && (!f0.num_elements || !f1.num_elements))) return plan;
    const uint32_t n0 = f0.num_elements, n1 = f1.num_elements;
    std::vector<int> mapping;
    std::vector<uint8_t> swaps(n0, 0), used(n1, 0), fading(n0, 0), births(n1, 0);
    std::vector<MatrixPlan> coherent;
    if (plan->trajectory) mapping = BuildLocalCorrespondence(prev, f0, f1);
    else {
        mapping = BuildOrdinaryCorrespondence(f0, f1);
        for (uint32_t i = 0; i < n0; ++i)
            swaps[i] = mapping[i] >= 0 && f0.elements[i].frame_num != f1.elements[mapping[i]].frame_num;
    }
    if (menuBats) {
        if (!MapMenuBats(prev, f0, f1, mapping, swaps, fading, births, coherent)) {
            plan->bypass = true;
            return plan;
        }
        plan->trajectory = true;
    } else if (plan->trajectory) {
        for (uint32_t i = 0; i < n0; ++i) fading[i] = mapping[i] < 0;
    }
    for (uint32_t i = 0; i < n0; ++i)
        if (mapping[i] >= 0) used[mapping[i]] = 1;
    if (plan->trajectory && !menuBats)
        for (uint32_t j = 0; j < n1; ++j) births[j] = !used[j];
    plan->firstOrder.assign(n0, -1);
    plan->secondOrder.assign(n1, -1);
    const auto append = [&](uint32_t index, bool target) {
        const auto& start = target ? f1.elements[index] : f0.elements[index];
        const int match = target ? static_cast<int>(index) : mapping[index];
        MatrixPlan matrix;
        bool swap = false;
        uint32_t sprite = start.frame_num;
        if (!target && !coherent.empty() && coherent[index].mode) {
            matrix = coherent[index];
        } else if (target || fading[index]) {
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
        const auto slot = static_cast<int>(plan->matrices.size());
        if (target) plan->secondOrder[index] = slot;
        else {
            plan->firstOrder[index] = slot;
            if (match >= 0) plan->secondOrder[match] = slot;
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
                if (births[nextBirth]) append(nextBirth, true);
                ++nextBirth;
            }
        }
        append(i, false);
    }
    if (plan->trajectory)
        for (; nextBirth < n1; ++nextBirth)
            if (births[nextBirth]) append(nextBirth, true);
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
