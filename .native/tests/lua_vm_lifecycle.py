"""实际 LuaJIT 与 Win32 的 .pkg 加载及 VM 生命周期测试，不启动游戏。

参数：lua51DS.dll 路径、模组根目录、vm_native_fixture.dll 路径。
测试数据放在测试 DLL 所在的项目临时目录内；每个场景使用独立进程。
"""
import ctypes
import pathlib
import shutil
import subprocess
import sys


lua_path, mod_root, fixture_path = [pathlib.Path(p).resolve() for p in sys.argv[1:4]]
if len(sys.argv) == 4:
    for case in ("local", "workshop", "missing", "corrupt", "locked", "foreign"):
        subprocess.run([sys.executable, __file__, str(lua_path), str(mod_root), str(fixture_path), case], check=True)
    print("PASS: all real LuaJIT/Win32 package and VM lifecycle cases")
    sys.exit(0)

case = sys.argv[4]
test_root = fixture_path.parent / ("case-" + case)
if case == "workshop":
    workshop_root = test_root / "steamapps/workshop/content/322330"
    physical_root = workshop_root / "123456789"
    virtual_root = test_root / "virtual-mods/workshop-123456789"
else:
    physical_root = test_root / "本地模组 空格"
    virtual_root = physical_root
physical_root.mkdir(parents=True, exist_ok=True)
package = physical_root / "dst_animation_tween.dll.pkg"
runtime = physical_root / "dst_animation_tween.dll"
if case != "missing":
    shutil.copyfile(fixture_path, package)
if case == "corrupt":
    package.write_bytes(b"broken-package")
if case in ("missing", "corrupt", "locked"):
    runtime.write_bytes(b"keep-original")

lua = ctypes.CDLL(str(lua_path))
lua.luaL_newstate.argtypes = []
lua.luaL_newstate.restype = ctypes.c_void_p
for name in ("luaL_openlibs", "lua_close"):
    fn = getattr(lua, name)
    fn.argtypes = [ctypes.c_void_p]
    fn.restype = None
lua.luaL_loadbuffer.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p]
lua.luaL_loadbuffer.restype = ctypes.c_int
lua.lua_pcall.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int]
lua.lua_pcall.restype = ctypes.c_int
lua.lua_tolstring.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p]
lua.lua_tolstring.restype = ctypes.c_char_p


def run(state, script):
    code = script.encode("utf-8")
    result = lua.luaL_loadbuffer(state, code, len(code), b"@vm_lifecycle")
    if not result:
        result = lua.lua_pcall(state, 0, 0, 0)
    if result:
        raise AssertionError(lua.lua_tolstring(state, -1, None).decode("utf-8"))


def new_vm():
    state = lua.luaL_newstate()
    assert state
    lua.luaL_openlibs(state)
    return state


setup = f"""
local info = {{}}
setfenv(assert(loadfile([=[{(mod_root / 'modinfo.lua').as_posix()}]=])), info)()
assert(not info.restart_required)
assert(info.author == '冰冰羊' and not info.configuration_options)
env = {{ GLOBAL = _G, MODROOT = [=[{virtual_root.as_posix()}/]=], modname = 'dst_animation_tween' }}
env.env = env
env.GetModConfigData = function() error('不应读取模组配置') end
function enter_mod()
    setfenv(assert(loadfile([=[{(mod_root / 'modmain.lua').as_posix()}]=])), env)()
end
"""
if case == "workshop":
    setup += f"""
env.modname = 'workshop-123456789'
GameInjector = {{ DS_LUAJIT_get_workshop_dir = function() return [=[{workshop_root.as_posix()}]=] end }}
"""

lock = None
if case == "locked":
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
    kernel.CreateFileW.restype = ctypes.c_void_p
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    kernel.CloseHandle.restype = ctypes.c_int
    lock = kernel.CreateFileW(str(runtime), 0x80000000, 1, None, 3, 0, None)
    assert lock not in (None, ctypes.c_void_p(-1).value)
if case == "foreign":
    other = test_root / "other/dst_animation_tween.dll"
    other.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(fixture_path, other)
    foreign_module = ctypes.CDLL(str(other))

state = new_vm()
try:
    run(state, setup)
    if case in ("missing", "corrupt", "locked", "foreign"):
        expected = {"missing": "没有找到", "corrupt": "DLL 包无效", "locked": "无法更新同目录 DLL", "foreign": "已加载另一目录"}[case]
        run(state, f"local message; print = function(text) message = text end; enter_mod(); assert(not DST_ANIMATION_TWEEN); assert(message:find('{expected}', 1, true), message)")
        if case != "foreign":
            assert runtime.read_bytes() == b"keep-original", "失败时不得破坏已有 DLL"
        assert not list(physical_root.glob("*.tmp")), "失败后应清理暂存文件"
    else:
        run(state, "enter_mod(); assert(DST_ANIMATION_TWEEN)")
        assert runtime.read_bytes() == fixture_path.read_bytes()
        native = ctypes.CDLL(str(runtime))
        native.HS_ReadStats.argtypes = [ctypes.POINTER(ctypes.c_uint64), ctypes.c_uint32]
        native.HS_ReadStats.restype = ctypes.c_int

        def enabled():
            values = (ctypes.c_uint64 * 5)()
            assert native.HS_ReadStats(values, 5) == 1 and values[0] == 1
            return bool(values[1])

        assert enabled()
        run(state, "collectgarbage('collect'); collectgarbage('collect')")
        assert enabled(), "普通 GC 不应关闭补帧"
        run(state, "local api = DST_ANIMATION_TWEEN; enter_mod(); assert(api == DST_ANIMATION_TWEEN)")
        run(state, "DST_ANIMATION_TWEEN.SetEnabled(false)")
        assert not enabled()
        run(state, "DST_ANIMATION_TWEEN.SetEnabled(true)")
        assert enabled()
        # 运行副本被加载时，发布包仍应能被更新；下一 VM 复用现有模块。
        with package.open("r+b") as output:
            output.write(b"updated-on-disk")
finally:
    lua.lua_close(state)
    if lock:
        kernel.CloseHandle(lock)

if case in ("local", "workshop"):
    assert not enabled(), "VM 关闭后必须关闭补帧"
    state = new_vm()
    lua.lua_close(state)
    assert not enabled(), "停用模组的新 VM 应保持关闭"
    state = new_vm()
    try:
        # 此时 .pkg 已改变；同进程复用运行 DLL，不尝试覆盖已占用文件。
        run(state, setup + "enter_mod(); assert(DST_ANIMATION_TWEEN)")
        assert enabled()
        assert runtime.read_bytes() == fixture_path.read_bytes()
    finally:
        lua.lua_close(state)
    assert not enabled()
print("PASS:", case)
