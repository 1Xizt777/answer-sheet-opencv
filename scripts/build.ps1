# 编译答题卡 CLI 与工具
#
# 用法：.\scripts\build.ps1                                    # 只编 CLI
#       .\scripts\build.ps1 -Tools                             # 连工具一起编
#       .\scripts\build.ps1 -OpenCVRoot "D:\opencv\build"      # 指定 OpenCV 位置

[CmdletBinding()]
param(
    [switch]$Tools,
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Debug",
    # OpenCV 的 build 目录。优先级：参数 > 环境变量 OPENCV_ROOT > 默认值
    [string]$OpenCVRoot,
    [string]$Vcvars
)

$ErrorActionPreference = "Stop"

# ── 环境配置 ──
# 换机器时三种方式任选：改下面这行默认值 / 设环境变量 OPENCV_ROOT / 传 -OpenCVRoot 参数
$DefaultOpenCVRoot = "E:\Opencv\opencv\build"

if (-not $OpenCVRoot) { $OpenCVRoot = $env:OPENCV_ROOT }
if (-not $OpenCVRoot) { $OpenCVRoot = $DefaultOpenCVRoot }

if (-not $Vcvars) {
    # 依次找 VS2022 各个版本
    $vsRoots = @("${env:ProgramFiles}\Microsoft Visual Studio\2022",
                 "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022")
    foreach ($vsRoot in $vsRoots) {
        foreach ($edition in @("Community", "Professional", "Enterprise", "BuildTools")) {
            $candidate = Join-Path $vsRoot "$edition\VC\Auxiliary\Build\vcvars64.bat"
            if (Test-Path $candidate) { $Vcvars = $candidate; break }
        }
        if ($Vcvars) { break }
    }
}

$root = Split-Path -Parent $PSScriptRoot

if (-not (Test-Path $OpenCVRoot)) {
    Write-Host "ERROR: 找不到 OpenCV：$OpenCVRoot" -ForegroundColor Red
    Write-Host ""
    Write-Host "请用以下任一方式告诉脚本 OpenCV 装在哪：" -ForegroundColor Yellow
    Write-Host "  1) 传参数    : .\scripts\build.ps1 -OpenCVRoot `"D:\opencv\build`""
    Write-Host "  2) 设环境变量: `$env:OPENCV_ROOT = `"D:\opencv\build`""
    Write-Host "  3) 改默认值  : 编辑 scripts\build.ps1 里的 `$DefaultOpenCVRoot"
    Write-Host ""
    Write-Host "该路径应指向 OpenCV 的 build 目录（下有 include\ 和 x64\vc16\lib\）。"
    exit 1
}
if (-not (Test-Path $Vcvars)) {
    Write-Host "ERROR: 找不到 vcvars64.bat（需要 Visual Studio 2022 的 C++ 工具链）" -ForegroundColor Red
    Write-Host "请用 -Vcvars 指定，例如：" -ForegroundColor Yellow
    Write-Host '  .\scripts\build.ps1 -Vcvars "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"'
    exit 1
}

# 从 lib 目录自动推出要链接的库名（opencv_world500.lib / opencv_world500d.lib 等）
$libDir = Join-Path $OpenCVRoot "x64\vc16\lib"
if (-not (Test-Path $libDir)) {
    Write-Host "ERROR: 找不到库目录 $libDir" -ForegroundColor Red
    Write-Host "请确认 -OpenCVRoot 指向 OpenCV 的 build 目录，且是 vc16/x64 版本。" -ForegroundColor Yellow
    exit 1
}
$allLibs = @(Get-ChildItem $libDir -Filter "opencv_world*.lib" -ErrorAction SilentlyContinue)
if ($Config -eq "Debug") {
    $foundLib = $allLibs | Where-Object { $_.BaseName -match 'd$' } | Select-Object -First 1
} else {
    $foundLib = $allLibs | Where-Object { $_.BaseName -notmatch 'd$' } | Select-Object -First 1
}
if (-not $foundLib) {
    $seen = if ($allLibs.Count -gt 0) { ($allLibs | ForEach-Object { $_.BaseName }) -join ', ' } else { 'nothing' }
    Write-Host "ERROR: 在 $libDir 下找不到 $Config 版 opencv_world*.lib" -ForegroundColor Red
    Write-Host "该目录下的库: $seen" -ForegroundColor Yellow
    exit 1
}
$libName = $foundLib.Name

$mdFlag = if ($Config -eq "Debug") { "/MDd /D_DEBUG" } else { "/MD /DNDEBUG" }
$outDir = Join-Path $root "x64\$Config"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$targets = @()
$targets += @{ Src = Join-Path $root "Answer sheet\Answer sheet.cli.cpp"; Out = "answersheet_cli" }

if ($Tools) {
    $targets += @{ Src = Join-Path $root "tools\gen_samples.cpp"; Out = "gen_samples" }
    $targets += @{ Src = Join-Path $root "tools\gen_stress.cpp";  Out = "gen_stress" }
    $targets += @{ Src = Join-Path $root "tools\diag.cpp";        Out = "diag" }
}

foreach ($t in $targets) {
    Write-Host "building $($t.Out) ..." -ForegroundColor Cyan
    $bat = @"
@echo off
call "$Vcvars" >nul || exit 1
cl /nologo /std:c++17 /EHsc /utf-8 $mdFlag /I"$OpenCVRoot\include" "$($t.Src)" /Fe:"$outDir\$($t.Out).exe" /Fo:"$outDir\$($t.Out).obj" /link /LIBPATH:"$OpenCVRoot\x64\vc16\lib" $libName
"@
    $tmpBat = Join-Path $env:TEMP "as_build_$($t.Out).bat"
    Set-Content -Path $tmpBat -Value $bat -Encoding ASCII
    cmd /c $tmpBat
    if ($LASTEXITCODE -ne 0) { Write-Host "FAILED: $($t.Out)" -ForegroundColor Red; exit 1 }
}

# 复制运行所需 DLL
$openBin = Join-Path $OpenCVRoot "x64\vc16\bin"
foreach ($dll in Get-ChildItem $openBin -Filter "opencv_world*.dll" -ErrorAction SilentlyContinue) {
    Copy-Item $dll.FullName $outDir -Force
}

Write-Host ""
Write-Host "ok -> $outDir" -ForegroundColor Green
