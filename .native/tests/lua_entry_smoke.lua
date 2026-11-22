local root = arg[1]
assert(loadfile(root .. "/modinfo.lua"))
assert(loadfile(root .. "/modmain.lua"))
local info = {}
setfenv(assert(loadfile(root .. "/modinfo.lua")), info)()
assert(info.author == "冰冰羊" and not info.configuration_options)
local function run(label, native, previous)
    local globals, lines, imports = {}, {}, 0
    for k, v in pairs(_G) do globals[k] = v end
    globals.print = function(value) lines[#lines + 1] = value end
    globals.TheNet = {IsDedicated = function() return label == "dedicated" end}
    globals.require = function(name)
        imports = imports + 1
        if label == "no_ffi" then error("no ffi") end
        assert(name == "ffi")
        return {
            os = label == "linux" and "Linux" or "Windows",
            abi = function() return label ~= "32bit" end,
            cdef = function() end,
            C = {
                MultiByteToWideChar = function(_, _, text, _, buffer)
                    if buffer then buffer.text = text end
                    return #text + 1
                end,
                WideCharToMultiByte = function(_, _, wide, _, buffer)
                    if buffer then buffer.text = wide.text end
                    return #wide.text + 1
                end,
                GetFullPathNameW = function(wide, _, buffer)
                    buffer.text = wide.text
                    return #wide.text
                end,
                GetFileAttributesW = function(path)
                    assert(path.text:match("dst_animation_tween%.dll%.pkg$"))
                    return label == "missing_pkg" and 4294967295 or 0
                end,
                GetModuleHandleA = function() return nil end,
                GetTempFileNameW = function(_, _, _, output) output.text = "stage.tmp"; return 1 end,
                CopyFileW = function() return 1 end,
                LoadLibraryExW = function() return {} end,
                FreeLibrary = function() return 1 end,
                MoveFileExW = function() return 1 end,
                LoadLibraryW = function(path)
                    assert(path.text:match("dst_animation_tween%.dll$"))
                    return {}
                end,
                GetProcAddress = function(_, symbol) return native[symbol] end,
                GetLastError = function() return 0 end,
            },
            cast = function(_, value) return value end,
            string = function(value) return type(value) == "table" and value.text or value end,
            new = function(ctype)
                if ctype == "uint64_t[6]" then return {[0]=1,1,2,3,4,5} end
                return {}
            end,
            gc = function(value, finalizer) value.finalizer = finalizer; return value end,
        }
    end
    globals.DST_ANIMATION_TWEEN = previous
    local environment = { GLOBAL=globals, MODROOT="../mods/dst_animation_tween/", modname="dst_animation_tween" }
    environment.env = environment
    environment.GetModConfigData = function() error("不应读取模组配置") end
    setfenv(assert(loadfile(root .. "/modmain.lua")), environment)()
    if label == "dedicated" then assert(imports == 0) end
    return globals, lines
end
for _, label in ipairs({"dedicated","no_ffi","linux","32bit","missing_pkg"}) do run(label) end
local count, enabled = 0
local native = {
    HS_Version = function() return info.version end,
    HS_LastError = function() return "signature_not_unique_or_missing" end,
    HS_Initialize = function() count=count+1; return 1 end,
    HS_SetEnabled = function(value) enabled=value; return 1 end,
    HS_ReadStats = function() return 1 end,
}
local globals = run("enabled",native)
assert(count == 1 and enabled == 1 and globals.DST_ANIMATION_TWEEN)
local stats = globals.DST_ANIMATION_TWEEN.GetStats()
assert(stats.installed and stats.enabled and stats.interpolated_frames == 2)
run("repeat",native,globals.DST_ANIMATION_TWEEN)
assert(count == 1 and enabled == 1)
globals.DST_ANIMATION_TWEEN.SetEnabled(false)
assert(enabled == 0)
globals.DST_ANIMATION_TWEEN.SetEnabled(true)
globals.DST_ANIMATION_TWEEN._vm_lifetime.finalizer()
assert(enabled == 0)
native.HS_Initialize = function() return 0 end
assert(not run("rejected",native).DST_ANIMATION_TWEEN)
native.HS_Version = function() return "old" end
assert(not run("old",native).DST_ANIMATION_TWEEN)
print("PASS: Lua syntax, platform gates, missing package, initialization, toggle, stats, repeat entry and failures (mock only)")
