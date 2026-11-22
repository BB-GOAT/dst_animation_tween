constexpr uint8_t kFrameGeometryBranch[] = {
    0x48,0x8B,0x8B,0xB8,0x00,0x00,0x00,0x4D,0x8B,0xEF,0x4C,0x39,
    0xB9,0xB0,0x00,0x00,0x00,0x74,0x58,0x4C,0x39,0xBB,0x70,0x01,
    0x00,0x00,0x75,0x4F,0x48,0x8B,0x83,0x50,0x01,0x00,0x00,0x48,
    0x2B,0x83,0x48,0x01,0x00,0x00,0x48,0xA9,0xF0,0xFF,0xFF,0xFF,
    0x74,0x27,0x48,0x8B,0x83,0x50,0x01,0x00,0x00,0x48,0x2B,0x83,
    0x48,0x01,0x00,0x00,0x48,0x83,0xE0,0xF0,0x48,0x83,0xF8,0x10,
    0x75,0x21,0x48,0x8B,0x83,0x48,0x01,0x00,0x00,0x81,0x38,0x03,
    0x8B,0x29,0x1D,0x75,0x12,
};

// 仅适用于已核对的原始帧几何分支；帧编号同时决定顶点范围和矩阵槽次序。
struct FrameGeometryLayout {
    bool verified = false;
};
FrameGeometryLayout g_frameGeometry;
volatile LONG64 g_frameGeometryFrames = 0;

static bool VerifyFrameGeometryLayout(const uint8_t* text, size_t size, FrameGeometryLayout& result) {
    // 当前客户端完整分支：Build几何存在、无替代绘制数据，且符号覆盖表为空或为允许的单项。
    unsigned matches = 0;
    if (size >= sizeof(kFrameGeometryBranch))
        for (size_t i = 0; i <= size - sizeof(kFrameGeometryBranch); ++i)
            if (text[i] == kFrameGeometryBranch[0] &&
                !memcmp(text + i, kFrameGeometryBranch, sizeof(kFrameGeometryBranch))) ++matches;
    result.verified = matches == 1;
    return result.verified;
}

template<class T> static T ReadGeometryField(const void* object, size_t offset) noexcept {
    T result;
    memcpy(&result, static_cast<const uint8_t*>(object) + offset, sizeof(result));
    return result;
}

static bool UsesFrameGeometry(const void* command) noexcept {
    if (!command || !g_frameGeometry.verified) return false;
    const auto* build = ReadGeometryField<const uint8_t*>(command, 0xB8);
    if (!build || !ReadGeometryField<const void*>(build, 0xB0) ||
        ReadGeometryField<const void*>(command, 0x170)) return false;
    const auto* begin = ReadGeometryField<const uint8_t*>(command, 0x148);
    const auto* end = ReadGeometryField<const uint8_t*>(command, 0x150);
    if (begin == end) return true;
    return begin && reinterpret_cast<uintptr_t>(end) - reinterpret_cast<uintptr_t>(begin) == 16 &&
           ReadGeometryField<uint32_t>(begin, 0) == 0x1D298B03u;
}
