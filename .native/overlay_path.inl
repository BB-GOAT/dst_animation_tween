// 已核对当前客户端递归RenderFrame调用：4个寄存器参数、22个栈参数。
// 包装器只维护挂接路径，绘制参数和递归函数保持原样。
using OverlayRender_t = void (*)(
    uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6,
    uintptr_t a7, uintptr_t a8, uintptr_t a9, uintptr_t a10, uintptr_t a11, uintptr_t a12,
    uintptr_t a13, uintptr_t a14, uintptr_t a15, uintptr_t a16, uintptr_t a17, uintptr_t a18,
    uintptr_t a19, uintptr_t a20, uintptr_t a21, uintptr_t a22, uintptr_t a23, uintptr_t a24,
    uintptr_t a25, uintptr_t a26);
OverlayRender_t g_origOverlayRender = nullptr;
static void RenderOverlayWithPath(
    uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6,
    uintptr_t a7, uintptr_t a8, uintptr_t a9, uintptr_t a10, uintptr_t a11, uintptr_t a12,
    uintptr_t a13, uintptr_t a14, uintptr_t a15, uintptr_t a16, uintptr_t a17, uintptr_t a18,
    uintptr_t a19, uintptr_t a20, uintptr_t a21, uintptr_t a22, uintptr_t a23, uintptr_t a24,
    uintptr_t a25, uintptr_t a26) {
    OverlayPathScope scope;
    g_origOverlayRender(
        a1, a2, a3, a4, a5, a6,
        a7, a8, a9, a10, a11, a12,
        a13, a14, a15, a16, a17, a18,
        a19, a20, a21, a22, a23, a24,
        a25, a26);
}
