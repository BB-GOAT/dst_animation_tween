// 隔离进程验证补帧核心、开关回退、三通道 relay 和发布逻辑；不执行游戏代码。
#include "../animation_tween.cpp"
#include <cassert>

const void* Original(const void* animation, int32_t, float) { return animation; }

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
    InterlockedExchange(&g_skeletalTweenEnabled, 1);
    assert(InterpGetFrameCore(&animation,1,0,0x1000,true) == &frames[0]);
    InterpGetFrameCore(&animation,1,1.0f/30,0x1000,true);
    Sleep(10);
    auto* result=static_cast<const AnimFrame*>(InterpGetFrameCore(&animation,1,1.0f/30,0x1000,true));
    assert(result && result->num_elements==1 && result->elements);
    const float x=HalfToFloat(result->elements[0].m[4]);
    assert(x>=0 && x<=32);
    InterlockedExchange(&g_skeletalTweenEnabled,0);
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
    assert(Publish(hooks));
    for (size_t i=0;i<3;++i) {
        captured=0;
        auto call=reinterpret_cast<const void*(*)(const void*,int32_t,float)>(code+i*64);
        call(nullptr,0,0);
        assert(captured==(i==0?context[1]:reinterpret_cast<uintptr_t>(context)));
    }
    assert(!Publish(hooks)); // 已修改的调用点不能再次覆盖。
    for(auto& hook:hooks) VirtualFree(hook.stub,0,MEM_RELEASE);
    VirtualFree(code,0,MEM_RELEASE);
}

int main() {
    CheckCore();
    CheckPublish();
    uint64_t stats[5]{};
    assert(HS_ReadStats(stats,5)==1 && stats[0]==0 && stats[1]==0);
    assert(HS_ReadStats(stats,4)==0);
    assert(HS_Initialize()==0 && !strcmp(HS_LastError(),"unsupported_executable"));
    puts("PASS: core guards, interpolation, disabled fallback, three relays, publication recheck, stats and executable rejection");
}
