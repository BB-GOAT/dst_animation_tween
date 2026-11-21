// 仅用于独立进程中的 VM 生命周期测试，不修改游戏内存或安装钩子。
#include "../animation_tween.cpp"

// 测试加载器会直接复制并加载本文件；模拟已安装状态以测试真实的开关导出。
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) g_installed = true;
    return TRUE;
}
