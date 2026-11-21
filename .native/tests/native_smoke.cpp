// 隔离进程验证补帧核心、开关回退、三通道 relay 和发布逻辑；不执行游戏代码。
#include "../animation_tween.cpp"
#include <cassert>

const AnimFrame* forcedOriginal = nullptr;
const void* Original(const void* animation, int32_t, float) { return forcedOriginal ? forcedOriginal : animation; }

void CheckCore() {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    g_qpcInvFreq = 1.0 / static_cast<double>(frequency.QuadPart);
    g_origAnimGetFrame = Original;
    int token = 1;
    InterlockedExchange(&g_skeletalTweenEnabled, 0);
    assert(FastInterpGetFrameMain(&token, 0, 0, &token) == &token);
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

// 生成小型 x64 调用夹具，检验 Main 的 [rbx+8] 和另外两路的 rbx 协议。
uintptr_t captured = 0;
const void* Capture(const void*,int32_t,float,const void* key) {
    captured=reinterpret_cast<uintptr_t>(key);
    return key;
}
void CheckPublish() {
    auto* code=static_cast<uint8_t*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));
    assert(code);
    uintptr_t context[2]={0x1234,0x5678};
    std::array<Hook,3> hooks{};
    for (size_t i=0;i<3;++i) {
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
        hooks[i].replacement=reinterpret_cast<const void*>(Capture); hooks[i].parent=i==0;
        assert(PrepareStub(hooks[i]));
    }
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
    // 四处修改必须统一拒绝：任一目标不符时不能发布其他钩子。
    identity.site[0] ^= 1;
    assert(!Publish(hooks, &identity));
    assert(!memcmp(hooks[0].call + 1, &hooks[0].before, 4));
    identity.site[0] ^= 1;
    assert(Publish(hooks, &identity));
    for (size_t i=0;i<3;++i) {
        captured=0;
        auto call=reinterpret_cast<const void*(*)(const void*,int32_t,float)>(code+i*64);
        call(nullptr,0,0);
        assert(captured==(i==0?context[1]:reinterpret_cast<uintptr_t>(context)));
    }
    auto identityCall = reinterpret_cast<const AnimFrame*(*)(const AnimFrame*)>(identityCaller);
    AnimFrame source{};
    auto& slot = g_tlsAnimState->slots[0];
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
    for(auto& hook:hooks) VirtualFree(hook.stub,0,MEM_RELEASE);
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

int main() {
    CheckCore();
    CheckPublish();
    CheckIdentityRegisters();
    uint64_t stats[5]{};
    assert(HS_ReadStats(stats,5)==1 && stats[0]==0 && stats[1]==0);
    assert(HS_ReadStats(stats,4)==0);
    assert(HS_Initialize()==0 && !strcmp(HS_LastError(),"unsupported_executable"));
    puts("PASS: interpolation, original frame identity/index, source immutability, four-site publication, relays, unwind, stats and executable rejection");
}
