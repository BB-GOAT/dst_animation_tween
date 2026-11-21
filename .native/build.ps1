param(
    [string]$BuildRoot = 'E:\Codex\dst_animation_tween\build',
    [switch]$Test
)
$ErrorActionPreference = 'Stop'
$nativeRoot = $PSScriptRoot
$modRoot = Split-Path -Parent $nativeRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) { throw '未找到 vswhere.exe，请安装 Visual Studio C++ x64 构建工具。' }
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw '未找到 MSVC x64 工具链。' }
$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
$null = New-Item -ItemType Directory -Path $BuildRoot -Force
$buildPath = (Resolve-Path -LiteralPath $BuildRoot).Path
# 工具链和源文件路径只写入固定格式的批处理；构建产物统一放在项目临时目录。
foreach ($path in @($nativeRoot, $buildPath, $vcvars)) {
    if ($path.IndexOfAny([char[]]'%"&|<>^') -ge 0) { throw "不支持的构建路径字符：$path" }
}
$lines = @(
    '@echo off',
    "call `"$vcvars`" >nul",
    'if errorlevel 1 exit /b %errorlevel%',
    "cd /d `"$buildPath`"",
    "cl /nologo /std:c++17 /EHsc /O2 /MD /utf-8 /W4 /LD `"$nativeRoot\animation_tween.cpp`" /link /OUT:dst_animation_tween.dll /INCREMENTAL:NO /OPT:REF /OPT:ICF",
    'if errorlevel 1 exit /b %errorlevel%'
)
if ($Test) {
    $lines += @(
        "cl /nologo /std:c++17 /EHsc /O2 /MD /utf-8 /W4 `"$nativeRoot\tests\native_smoke.cpp`" /Fe:native_smoke.exe /link /INCREMENTAL:NO",
        'if errorlevel 1 exit /b %errorlevel%',
        'native_smoke.exe',
        'if errorlevel 1 exit /b %errorlevel%'
    )
}
$commandFile = Join-Path $buildPath 'compile.cmd'
[IO.File]::WriteAllLines($commandFile, $lines, [Text.UTF8Encoding]::new($false))
& $env:ComSpec /d /c $commandFile
if ($LASTEXITCODE -ne 0) { throw "原生构建或测试失败，退出码 $LASTEXITCODE。" }
$destination = Join-Path $modRoot 'dst_animation_tween.dll.pkg'
Copy-Item -LiteralPath (Join-Path $buildPath 'dst_animation_tween.dll') -Destination $destination -Force
Get-FileHash -LiteralPath $destination -Algorithm SHA256
Write-Host '已构建 .dll.pkg 发布包；运行副本由模组生成，新版 DLL 在完全退出游戏后加载。'
