// 动画补帧核心位于 skeletal_core.inl；本文件负责加载、校验与控制。
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <array>

namespace {
using AnimGetFrame_t = const void* (*)(const void*, int32_t, float);
AnimGetFrame_t g_origAnimGetFrame = nullptr;
double g_qpcInvFreq = 0.0;
volatile LONG g_skeletalTweenEnabled = 0;
volatile LONG64 g_animInterpFrames = 0;
volatile LONG64 g_animElemsTweened = 0;
volatile LONG64 g_animPass2Swaps = 0;
SRWLOCK g_initLock = SRWLOCK_INIT;
bool g_installed = false;
char g_lastError[256] = "not_initialized";
std::wstring g_logPath;

#include "skeletal_core.inl"

void Log(const char* format, ...) {
    FILE* file = nullptr;
    if (g_logPath.empty() || _wfopen_s(&file, g_logPath.c_str(), L"ab") || !file) return;
    SYSTEMTIME time{};
    GetLocalTime(&time);
    fprintf(file, "[%02u:%02u:%02u.%03u] ", time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);
    fputc('\n', file);
    fclose(file);
}

bool Fail(const char* error) {
    strncpy_s(g_lastError, error, _TRUNCATE);
    Log("initialize_rejected reason=%s win32=%lu", error, GetLastError());
    return false;
}

bool PrepareLog() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&PrepareLog), &self)) return false;
    wchar_t path[32768]{};
    const DWORD count = GetModuleFileNameW(self, path, 32768);
    if (!count || count >= 32768) return false;
    std::wstring root(path, count);
    auto slash = root.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    root.resize(slash); // DLL 位于模组根目录，日志直接写入其 logs 子目录。
    root += L"\\logs";
    if (!CreateDirectoryW(root.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    g_logPath = root + L"\\animation_tween.log";
    FILE* file = nullptr;
    if (_wfopen_s(&file, g_logPath.c_str(), L"ab") || !file) return false;
    fclose(file);
    return true;
}

bool GetTextSection(uint8_t*& start, size_t& size) {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    if (!base) return false;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return false;
    auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (memcmp(section->Name, ".text", 5) == 0) {
            start = base + section->VirtualAddress;
            size = section->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

struct Hook {
    const char* name;
    const uint8_t* pre;
    size_t preSize;
    const uint8_t* post;
    size_t postSize;
    const void* replacement;
    bool parent;
    uint8_t* call = nullptr;
    uint8_t* stub = nullptr;
    int32_t before = 0;
    int32_t after = 0;
};

// 检查调用点签名唯一命中，以及三个原调用目标一致。
constexpr uint8_t preMain[] = {0x8B,0x93,0xC0,0,0,0,0x48,0x8B,0x8B,0xB0,0,0,0,0xF3,0x0F,0x10,0x93,0xC4,0,0,0,0xE8};
constexpr uint8_t postMain[] = {0x4C,0x8B,0xF0,0x48,0x89,0x84,0x24};
constexpr uint8_t preOverlay[] = {0x8B,0x53,0x10,0x48,0x8B,0x0B,0xF3,0x0F,0x10,0x53,0x14,0xE8};
constexpr uint8_t postOverlay[] = {0x48,0x8B,0xF0,0x48,0x85,0xC0,0x0F,0x84};
constexpr uint8_t preUI[] = {0x8B,0x93,0x14,0x01,0,0,0xF3,0x0F,0x10,0x93,0x18,0x01,0,0,0xE8};
constexpr uint8_t postUI[] = {0x48,0x8B,0xE8,0x48,0x85,0xC0,0x0F,0x84};

bool FindUnique(Hook& hook, uint8_t* start, size_t size) {
    const size_t total = hook.preSize + 4 + hook.postSize;
    unsigned matches = 0;
    if (size < total) return false;
    for (size_t offset = 0; offset <= size - total; ++offset) {
        if (start[offset] == hook.pre[0] && !memcmp(start + offset, hook.pre, hook.preSize)
            && !memcmp(start + offset + hook.preSize + 4, hook.post, hook.postSize)) {
            hook.call = start + offset + hook.preSize - 1;
            ++matches;
        }
    }
    Log("signature channel=%s matches=%u call=%p", hook.name, matches, hook.call);
    return matches == 1;
}

uint8_t* AllocNearExecutable(uint8_t* nearAddress) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(nearAddress) & ~uintptr_t(0xFFFF);
    for (uintptr_t delta = 0x10000; delta < 0x60000000; delta += 0x10000) {
        if (base > delta) {
            void* p = VirtualAlloc(reinterpret_cast<void*>(base - delta), 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (p) return static_cast<uint8_t*>(p);
        }
        void* p = VirtualAlloc(reinterpret_cast<void*>(base + delta), 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (p) return static_cast<uint8_t*>(p);
    }
    return nullptr;
}

bool PrepareStub(Hook& hook) {
    hook.stub = AllocNearExecutable(hook.call);
    if (!hook.stub) return false;
    const uint64_t destination = reinterpret_cast<uint64_t>(hook.replacement);
    if (hook.parent) {
        uint8_t code[18] = {0x4C,0x8B,0x4B,0x08,0xFF,0x25,0,0,0,0};
        memcpy(code + 10, &destination, 8);
        memcpy(hook.stub, code, sizeof(code));
    } else {
        uint8_t code[17] = {0x49,0x89,0xD9,0xFF,0x25,0,0,0,0};
        memcpy(code + 9, &destination, 8);
        memcpy(hook.stub, code, sizeof(code));
    }
    const int64_t delta = reinterpret_cast<intptr_t>(hook.stub) - reinterpret_cast<intptr_t>(hook.call + 5);
    if (delta < INT32_MIN || delta > INT32_MAX) return false;
    hook.after = static_cast<int32_t>(delta);
    DWORD old = 0;
    return VirtualProtect(hook.stub, 64, PAGE_EXECUTE_READ, &old)
        && FlushInstructionCache(GetCurrentProcess(), hook.stub, 64);
}

// FFI 初始化可能发生在 smooth_loading 正在刷新画面时。
// 所有分配和日志先完成，只在发布三个 call 位移期间暂停其他现存线程；
// 暂停区不调用 Lua、日志、堆分配或加载器，失败时恢复原位移并恢复线程。
class PausedThreads {
    std::vector<HANDLE> handles;
    size_t paused = 0;
public:
    bool Pause() {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return false;
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        BOOL more = Thread32First(snapshot, &entry);
        bool okay = more != FALSE;
        while (more) {
            if (entry.th32OwnerProcessID == GetCurrentProcessId() && entry.th32ThreadID != GetCurrentThreadId()) {
                HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, entry.th32ThreadID);
                if (!thread) { okay = false; break; }
                handles.push_back(thread);
            }
            more = Thread32Next(snapshot, &entry);
        }
        CloseHandle(snapshot);
        if (!okay) return false;
        for (HANDLE thread : handles) {
            if (SuspendThread(thread) == DWORD(-1)) return false;
            ++paused;
        }
        // GetThreadContext 等到线程真正暂停，避免只递增挂起计数就开始改写。
        for (HANDLE thread : handles) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;
            if (!GetThreadContext(thread, &context)) return false;
        }
        return true;
    }
    ~PausedThreads() {
        for (size_t i = paused; i > 0; --i) ResumeThread(handles[i - 1]);
        for (HANDLE thread : handles) CloseHandle(thread);
    }
};

bool Publish(std::array<Hook, 3>& hooks) {
    bool okay = false;
    {
        PausedThreads threads;
        if (!threads.Pause()) return false;
        for (const auto& h : hooks) {
            if (h.call[0] != 0xE8 || memcmp(h.call + 1, &h.before, 4)) return false;
        }
        size_t written = 0;
        bool completed = true;
        for (auto& h : hooks) {
            DWORD old = 0;
            if (!VirtualProtect(h.call + 1, 4, PAGE_EXECUTE_READWRITE, &old)) { completed = false; break; }
            memcpy(h.call + 1, &h.after, 4);
            ++written;
            const BOOL flushed = FlushInstructionCache(GetCurrentProcess(), h.call, 5);
            DWORD unused = 0;
            const BOOL restored = VirtualProtect(h.call + 1, 4, old, &unused);
            if (!flushed || !restored) { completed = false; break; }
        }
        okay = completed && written == hooks.size();
        if (!okay) {
            for (size_t i = 0; i < written; ++i) {
                auto& h = hooks[i];
                DWORD old = 0;
                if (VirtualProtect(h.call + 1, 4, PAGE_EXECUTE_READWRITE, &old)) {
                    memcpy(h.call + 1, &h.before, 4);
                    FlushInstructionCache(GetCurrentProcess(), h.call, 5);
                    DWORD unused = 0;
                    VirtualProtect(h.call + 1, 4, old, &unused);
                }
            }
        }
    }
    return okay;
}

bool Initialize() {
    if (!PrepareLog()) return Fail("log_open_failed");
    Log("initialize version=0.1.0");
    wchar_t executable[32768]{};
    const DWORD count = GetModuleFileNameW(nullptr, executable, 32768);
    if (!count || count >= 32768) return Fail("exe_path_failed");
    const auto* filename = wcsrchr(executable, L'\\');
    if (!filename || _wcsicmp(filename + 1, L"dontstarve_steam_x64.exe")) return Fail("unsupported_executable");
    uint8_t* start = nullptr;
    size_t size = 0;
    if (!GetTextSection(start, size)) return Fail("text_section_missing");
    std::array<Hook, 3> hooks{{
        {"main",preMain,sizeof(preMain),postMain,sizeof(postMain),reinterpret_cast<const void*>(&FastInterpGetFrameMain),true},
        {"overlay",preOverlay,sizeof(preOverlay),postOverlay,sizeof(postOverlay),reinterpret_cast<const void*>(&FastInterpGetFrameOverlay),false},
        {"ui",preUI,sizeof(preUI),postUI,sizeof(postUI),reinterpret_cast<const void*>(&FastInterpGetFrameUI),false}
    }};
    uint8_t* original = nullptr;
    for (auto& h : hooks) {
        if (!FindUnique(h, start, size)) return Fail("signature_not_unique_or_missing");
        memcpy(&h.before, h.call + 1, 4);
        const auto address = reinterpret_cast<intptr_t>(h.call + 5) + h.before;
        const auto begin = reinterpret_cast<intptr_t>(start);
        if (address < begin || address > begin + static_cast<intptr_t>(size) - 16) return Fail("original_target_outside_text");
        auto* target = reinterpret_cast<uint8_t*>(address);
        if (original && original != target) return Fail("original_targets_differ");
        original = target;
    }
    // 只接受已核对的函数序言，版本变化时记录并跳过。
    constexpr uint8_t prologue[] = {0x48,0x83,0xEC,0x28,0x4C,0x8B,0xC9,0xE8};
    Log("original=%p bytes=%02X%02X%02X%02X%02X%02X%02X%02X", original,
        original[0],original[1],original[2],original[3],original[4],original[5],original[6],original[7]);
    if (memcmp(original, prologue, sizeof(prologue))) return Fail("original_prologue_changed");
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) return Fail("qpc_unavailable");
    g_qpcInvFreq = 1.0 / static_cast<double>(frequency.QuadPart);
    g_origAnimGetFrame = reinterpret_cast<AnimGetFrame_t>(original);
    for (auto& h : hooks) {
        if (!PrepareStub(h)) {
            for (auto& p : hooks) if (p.stub) VirtualFree(p.stub, 0, MEM_RELEASE);
            return Fail("relay_prepare_failed");
        }
    }
    // 原生钩子跨 Lua VM 重建保留，必须固定 DLL，不能由 FFI GC 卸载。
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&Initialize), &pinned)) return Fail("module_pin_failed");
    if (!Publish(hooks)) return Fail("hook_publish_failed");
    g_installed = true;
    strcpy_s(g_lastError, "ok");
    Log("installed channels=main,overlay,ui algorithm=unchanged enabled=0");
    return true;
}
} // namespace

#define HS_EXPORT extern "C" __declspec(dllexport)
HS_EXPORT const char* HS_Version() { return "0.1.0"; }
HS_EXPORT const char* HS_LastError() { return g_lastError; }
HS_EXPORT int HS_Initialize() {
    AcquireSRWLockExclusive(&g_initLock);
    bool result = g_installed;
    try { if (!result) result = Initialize(); }
    catch (...) { Fail("initialize_exception"); }
    ReleaseSRWLockExclusive(&g_initLock);
    return result ? 1 : 0;
}
HS_EXPORT int HS_SetEnabled(int enabled) {
    if (!g_installed) return 0;
    InterlockedExchange(&g_skeletalTweenEnabled, enabled ? 1 : 0);
    Log("set_enabled value=%d", enabled ? 1 : 0);
    return 1;
}
// 读取补帧计数器，不增加逐调用计时或自动采样任务。
HS_EXPORT int HS_ReadStats(uint64_t* values, uint32_t count) {
    if (!values || count < 5) return 0;
    values[0] = g_installed ? 1 : 0;
    values[1] = InterlockedCompareExchange(&g_skeletalTweenEnabled, 0, 0) ? 1 : 0;
    values[2] = static_cast<uint64_t>(InterlockedCompareExchange64(&g_animInterpFrames, 0, 0));
    values[3] = static_cast<uint64_t>(InterlockedCompareExchange64(&g_animElemsTweened, 0, 0));
    values[4] = static_cast<uint64_t>(InterlockedCompareExchange64(&g_animPass2Swaps, 0, 0));
    return 1;
}
