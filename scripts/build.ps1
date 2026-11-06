# 编译答题卡 CLI 与工具
#
# 用法：.\scripts\build.ps1            # 只编 CLI
#       .\scripts\build.ps1 -Tools     # 连工具一起编

[CmdletBinding()]
param(
    [switch]$Tools,
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Debug"
)

$ErrorActionPreference = "Stop"

# ── 环境配置（换机器只改这两行）──
$OpenCVRoot = "E:\Opencv\opencv\build"
$Vcvars     = "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

$root = Split-Path -Parent $PSScriptRoot

if (-not (Test-Path $OpenCVRoot)) { Write-Host "ERROR: OpenCV not found at $OpenCVRoot" -ForegroundColor Red; exit 1 }
if (-not (Test-Path $Vcvars))     { Write-Host "ERROR: vcvars not found at $Vcvars" -ForegroundColor Red; exit 1 }

$libName = if ($Config -eq "Debug") { "opencv_world500d.lib" } else { "opencv_world500.lib" }
$mdFlag  = if ($Config -eq "Debug") { "/MDd /D_DEBUG" } else { "/MD /DNDEBUG" }
$outDir  = Join-Path $root "x64\$Config"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$targets = @()
$targets += @{ Src = Join-Path $root "Answer sheet\Answer sheet.cli.cpp"; Out = "answersheet_cli" }

if ($Tools) {
    $targets += @{ Src = Join-Path $root "tools\gen_samples.cpp"; Out = "gen_samples" }
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
foreach ($dll in Get-ChildItem $openBin -Filter "opencv_world*.dll" -ErrorAction SilentlyContinue) {
    Copy-Item $dll.FullName $outDir -Force
}

Write-Host ""
Write-Host "ok -> $outDir" -ForegroundColor Green
