GLOBAL.setmetatable(env, { __index = function(_, key) return GLOBAL.rawget(GLOBAL, key) end })

local G = GLOBAL
local prefix = "[动画补帧] "
if G.TheNet and G.TheNet:IsDedicated() then return end

local ok, ffi = G.pcall(G.require, "ffi")
if not ok or ffi.os ~= "Windows" or not ffi.abi("64bit") then
    G.print(prefix .. "需要 Windows x64 LuaJIT FFI，本次已跳过。")
    return
end

-- 只处理同一 Lua VM 中的重复入口；新 VM 会重新声明接口并连接常驻 DLL。
local existing = G.rawget(G, "DST_ANIMATION_TWEEN")
if existing then
    existing.SetEnabled(true)
    return
end

-- 路径解析与 .pkg 副本加载沿用 dst_inputfix 的机制。
local function LoadNative()
    local function Check(value, message)
        if not value then G.error(message, 0) end
        return value
    end

    ffi.cdef[[
        int __stdcall MultiByteToWideChar(unsigned int, unsigned long, const char*, int, unsigned short*, int);
        int __stdcall WideCharToMultiByte(unsigned int, unsigned long, const unsigned short*, int, char*, int, const char*, int*);
        unsigned long __stdcall GetFullPathNameW(const unsigned short*, unsigned long, unsigned short*, void*);
        unsigned long __stdcall GetModuleFileNameW(void*, unsigned short*, unsigned long);
        unsigned long __stdcall GetFileAttributesW(const unsigned short*);
        unsigned long __stdcall GetLastError(void);
        void* __stdcall LoadLibraryW(const unsigned short*);
        void* __stdcall GetModuleHandleA(const char*);
        void* __stdcall GetProcAddress(void*, const char*);
        unsigned int __stdcall GetTempFileNameW(const unsigned short*, const unsigned short*, unsigned int, unsigned short*);
        int __stdcall CopyFileW(const unsigned short*, const unsigned short*, int);
        int __stdcall MoveFileExW(const unsigned short*, const unsigned short*, unsigned long);
        int __stdcall DeleteFileW(const unsigned short*);
        void* __stdcall LoadLibraryExW(const unsigned short*, void*, unsigned long);
        int __stdcall FreeLibrary(void*);
    ]]
    local function Wide(text)
        local n = ffi.C.MultiByteToWideChar(65001, 8, text, -1, nil, 0)
        Check(n > 0, "DLL 路径不是有效 UTF-8")
        local buffer = ffi.new("unsigned short[?]", n)
        Check(ffi.C.MultiByteToWideChar(65001, 8, text, -1, buffer, n) > 0, "路径转换失败")
        return buffer
    end
    local function Utf8(wide)
        local n = ffi.C.WideCharToMultiByte(65001, 0, wide, -1, nil, 0, nil, nil)
        Check(n > 0, "路径转换失败")
        local buffer = ffi.new("char[?]", n)
        Check(ffi.C.WideCharToMultiByte(65001, 0, wide, -1, buffer, n, nil, nil) > 0, "路径转换失败")
        return ffi.string(buffer)
    end
    local function Full(path)
        local buffer = ffi.new("unsigned short[32768]")
        local n = ffi.C.GetFullPathNameW(Wide(path), 32768, buffer, nil)
        Check(n > 0 and n < 32768, "无法解析 DLL 绝对路径")
        return Utf8(buffer):gsub("\\", "/")
    end
    local candidates = { Full(MODROOT) }
    local id = G.tostring(modname):match("^workshop%-(%d+)$")
    local injector = G.rawget(G, "GameInjector")
    if id and injector and G.type(injector.DS_LUAJIT_get_workshop_dir) == "function" then
        local success, folder = G.pcall(injector.DS_LUAJIT_get_workshop_dir)
        if success and G.type(folder) == "string" and folder ~= "" then
            candidates[#candidates + 1] = Full(folder .. "/" .. id)
        end
    end
    local packagePath, runtimePath, modDir
    for _, candidate in G.ipairs(candidates) do
        local path = Full(candidate .. "/dst_animation_tween.dll.pkg")
        local attributes = G.tonumber(ffi.C.GetFileAttributesW(Wide(path)))
        if attributes ~= 4294967295 and G.math.floor(attributes / 16) % 2 == 0 then
            packagePath = path
            runtimePath = Full(candidate .. "/dst_animation_tween.dll")
            modDir = candidate
            break
        end
    end
    Check(packagePath, "没有找到本模组实体目录中的 dst_animation_tween.dll.pkg")
    -- 工坊只发布 .pkg；同目录 .dll 是本地生成的运行副本，不加入上传清单。
    local module = ffi.C.GetModuleHandleA("dst_animation_tween.dll")
    if module ~= nil then
        local buffer = ffi.new("unsigned short[32768]")
        local n = ffi.C.GetModuleFileNameW(module, buffer, 32768)
        Check(n > 0 and n < 32768 and Full(Utf8(buffer)):lower() == runtimePath:lower(),
            "已加载另一目录的 DLL，请完整退出游戏")
    else
        -- 在模组目录暂存并原子替换；既有 DLL 即使被其他进程占用也不会被截断。
        local temp = ffi.new("unsigned short[32768]")
        Check(ffi.C.GetTempFileNameW(Wide(modDir), Wide("dat"), 0, temp) ~= 0,
            "无法在模组目录创建 DLL 暂存文件，Win32=" .. G.tonumber(ffi.C.GetLastError()))
        local function StageCheck(value, message, code)
            if not value then
                ffi.C.DeleteFileW(temp)
                G.error(message .. "，Win32=" .. G.tonumber(code), 0)
            end
        end
        local copied = ffi.C.CopyFileW(Wide(packagePath), temp, 0) ~= 0
        StageCheck(copied, "复制 DLL 包失败", ffi.C.GetLastError())
        -- 只检查可映射性，不解析依赖、不执行 DllMain；验证后立即解除映射。
        local probe = ffi.C.LoadLibraryExW(temp, nil, 96) -- AS_IMAGE_RESOURCE | AS_DATAFILE_EXCLUSIVE
        StageCheck(probe ~= nil, "DLL 包无效或不完整", ffi.C.GetLastError())
        local unmapped = ffi.C.FreeLibrary(probe) ~= 0
        StageCheck(unmapped, "DLL 暂存检查清理失败", ffi.C.GetLastError())
        local replaced = ffi.C.MoveFileExW(temp, Wide(runtimePath), 9) ~= 0 -- REPLACE_EXISTING | WRITE_THROUGH
        StageCheck(replaced, "无法更新同目录 DLL，可能被另一游戏进程占用，请退出后重试", ffi.C.GetLastError())
        module = ffi.C.LoadLibraryW(Wide(runtimePath))
        Check(module ~= nil, "DLL 加载失败，Win32=" .. G.tonumber(ffi.C.GetLastError()))
    end
    local function Symbol(name, signature)
        local address = ffi.C.GetProcAddress(module, name)
        Check(address ~= nil, "DLL 缺少接口：" .. name)
        return ffi.cast(signature, address)
    end
    return {
        HS_Version = Symbol("HS_Version", "const char* (*)()"),
        HS_LastError = Symbol("HS_LastError", "const char* (*)()"),
        HS_Initialize = Symbol("HS_Initialize", "int (*)()"),
        HS_SetEnabled = Symbol("HS_SetEnabled", "int (*)(int)"),
        HS_ReadStats = Symbol("HS_ReadStats", "int (*)(uint64_t*, uint32_t)"),
    }
end

local loaded, native = G.pcall(LoadNative)
if not loaded then
    G.print(prefix .. "DLL 加载失败：" .. G.tostring(native))
    return
end
local version = ffi.string(native.HS_Version())
if version ~= "0.1.1" then
    G.print(prefix .. "DLL 版本不匹配，请完全退出游戏后重新构建或更新。")
    return
end
if native.HS_Initialize() ~= 1 then
    G.print(prefix .. "原生初始化已跳过：" .. ffi.string(native.HS_LastError())
        .. "；详情见模组 logs/animation_tween.log。")
    return
end

-- 当前 VM 存活期间由全局接口持有；VM 关闭时恢复原始取帧。
-- 停用模组后的新 VM 不会执行本文件，不能等下一次 modmain 再关闭补帧。
local api = {
    _vm_lifetime = ffi.gc(ffi.new("uint8_t[1]"), function()
        native.HS_SetEnabled(0)
    end),
}
function api.SetEnabled(value)
    local success = native.HS_SetEnabled(value and 1 or 0) == 1
    if not success then G.print(prefix .. "切换失败。") end
    return success
end
function api.GetStats()
    local values = ffi.new("uint64_t[5]")
    if native.HS_ReadStats(values, 5) ~= 1 then return end
    local result = {
        installed = values[0] ~= 0,
        enabled = values[1] ~= 0,
        interpolated_frames = G.tonumber(values[2]),
        tweened_elements = G.tonumber(values[3]),
        sprite_swap_matches = G.tonumber(values[4]),
    }
    G.print(prefix .. G.string.format("installed=%s enabled=%s interpolated_frames=%d tweened_elements=%d sprite_swap_matches=%d",
        G.tostring(result.installed), G.tostring(result.enabled), result.interpolated_frames,
        result.tweened_elements, result.sprite_swap_matches))
    return result
end
-- 当前 VM 持有原生接口；LoadLibrary 引用、DLL 和钩子常驻到进程退出。
G.rawset(G, "DST_ANIMATION_TWEEN", api)
api.SetEnabled(true)
