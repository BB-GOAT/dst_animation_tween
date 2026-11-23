---@diagnostic disable: lowercase-global
name = "DST动画补帧"
description = [[
给游戏动画补帧，需要配合Windows系统与Luajit模组才能使用
本模组不解锁帧率，请在LuaJIT模组中设置渲染帧率(并在游戏设置里调高刷新率)
模组更新后，请重启游戏以加载新版文件
]]
author = "冰冰羊"
version = "0.1.10"
api_version = 10
dst_compatible = true
client_only_mod = true
all_clients_require_mod = false
luajit_compatible = { dep_tailcall = false }

icon_atlas = "images/modicon.xml"
icon = "modicon.tex"