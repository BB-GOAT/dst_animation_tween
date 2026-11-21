name = "动画补帧"
description = [[
为主世界、附着动画及 UI 提供动画补帧，需要 Windows x64 LuaJIT FFI。
本模组不解锁帧率，请在现有 LuaJIT 补丁中设置帧率。
启用、停用随游戏重载生效；原生文件更新后，完全退出游戏再启动以加载新版。
]]
author = "冰冰羊"
version = "0.1.1"
api_version = 10
dst_compatible = true
client_only_mod = true
all_clients_require_mod = false
luajit_compatible = { dep_tailcall = false }
