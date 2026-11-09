# 压力测试：跑 gen_stress 产出的 N 张随机扰动图，量化准确率
#
# 用法：
#   .\scripts\run_stress.ps1                # 默认 40 张
#   .\scripts\run_stress.ps1 -Count 200     # 200 张
#   .\scripts\run_stress.ps1 -Regen         # 重新生成样本
#
# 输出：逐题准确率、整卡准确率、按扰动维度的退化曲线
# 退出码：0 = 达到门槛；1 = 未达标

[CmdletBinding()]
param(
    [int]$Count = 40,
    [int]$Seed = 20261109,
    [switch]$Regen,
    [string]$Exe = "",
    # 门槛：逐题准确率不低于这个值才算通过
    [double]$MinAccuracy = 0.95
)

$ErrorActionPreference = "Stop"
$root      = Split-Path -Parent $PSScriptRoot
$stressDir = Join-Path $root "data\stress"
$srcImage  = Join-Path $root "data\samples\slant_card.png"

if (-not $Exe) { $Exe = Join-Path $root "x64\Debug\answersheet_cli.exe" }

$opencvBin = "E:\Opencv\opencv\build\x64\vc16\bin"
if (Test-Path $opencvBin) { $env:PATH = "$opencvBin;$env:PATH" }

if (-not (Test-Path $Exe)) { Write-Host "ERROR: CLI not found: $Exe" -ForegroundColor Red; exit 1 }

$genExe = Join-Path $root "x64\Debug\gen_stress.exe"
$manifest = Join-Path $stressDir "manifest.json"

if ($Regen -or -not (Test-Path $manifest)) {
    if (-not (Test-Path $genExe)) { Write-Host "ERROR: gen_stress.exe not built (run build.ps1 -Tools)" -ForegroundColor Red; exit 1 }
    New-Item -ItemType Directory -Force -Path $stressDir | Out-Null
    Get-ChildItem $stressDir -Filter "stress_*.png" | Remove-Item -Force
    Write-Host "generating $Count samples (seed=$Seed) ..." -ForegroundColor Cyan
    & $genExe $stressDir $srcImage $Count $Seed | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Host "ERROR: gen_stress failed" -ForegroundColor Red; exit 1 }
}

$spec = [System.IO.File]::ReadAllText($manifest, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
$cases = @($spec.cases)
Write-Host "cases: $($cases.Count)" -ForegroundColor DarkGray

$tmpJson = Join-Path $env:TEMP "stress_out.json"

# 统计容器
$qTotal = 0; $qCorrect = 0; $qBlankTruth = 0; $qBlankRight = 0; $qWrongNotBlank = 0
$cardTotal = 0; $cardExact = 0; $notFound = 0
$buckets = @{}   # 维度 -> @{ label -> @{n, correct} }

function Add-Bucket($dim, $label, $ok) {
    if (-not $buckets.ContainsKey($dim)) { $buckets[$dim] = @{} }
    if (-not $buckets[$dim].ContainsKey($label)) { $buckets[$dim][$label] = @{ n = 0; correct = 0 } }
    $buckets[$dim][$label].n += 1
    if ($ok) { $buckets[$dim][$label].correct += 1 }
}

$i = 0
foreach ($c in $cases) {
    $i++
    if ($i % 25 -eq 0) { Write-Host "  ... $i / $($cases.Count)" -ForegroundColor DarkGray }

    $img = Join-Path $stressDir $c.image
    Remove-Item $tmpJson -ErrorAction SilentlyContinue

    & $Exe --image $img --out $tmpJson 2>&1 | Out-Null
    $exit = $LASTEXITCODE

    $cardTotal++
    if ($exit -ne 0 -or -not (Test-Path $tmpJson)) {
        $notFound++
        # 全错：真值里非空白的题都算错
        foreach ($t in $c.answers) { if ($null -ne $t) { $qTotal++; Add-Bucket "card" "not_found" $false } }
        continue
    }

    $actual = Get-Content $tmpJson -Raw -Encoding UTF8 | ConvertFrom-Json
    $act = @($actual.answers)
    $truth = @($c.answers)
    $allOk = $true

    for ($q = 0; $q -lt 6; $q++) {
        $t = if ($q -lt $truth.Count -and $null -ne $truth[$q]) { $truth[$q] } else { $null }
        $a = if ($q -lt $act.Count) { $act[$q] } else { "-" }

        # 空白题：实际必须是 '?'（程序内部用 '?' 表示未判定）
        if ($null -eq $t) {
            $qBlankTruth++
            $ok = ($a -eq '?')
            if ($ok) { $qBlankRight++ } else { $qWrongNotBlank++ }
        }
        else {
            $ok = ($a -eq $t)
        }

        $qTotal++
        if ($ok) { $qCorrect++ } else { $allOk = $false }

        # 按扰动维度分桶
        $scale = [double]$c.perturb.scale
        $angle = [math]::Abs([double]$c.perturb.angle)
        $fg    = [int]$c.perturb.fillGray
        $blur  = [double]$c.perturb.blurSigma
        $noise = [double]$c.perturb.noiseSigma

        $sLab = if ($scale -lt 0.55) { "0.45~0.55" } elseif ($scale -lt 0.70) { "0.55~0.70" } elseif ($scale -lt 0.90) { "0.70~0.90" } else { "0.90~1.10" }
        $aLab = if ($angle -lt 5) { "0~5deg" } elseif ($angle -lt 10) { "5~10deg" } else { "10~15deg" }
        $gLab = if ($fg -lt 100) { "<100 深" } elseif ($fg -lt 140) { "100~140 中" } else { ">140 浅" }
        $bLab = if ($blur -lt 0.5) { "0~0.5" } elseif ($blur -lt 1.0) { "0.5~1.0" } else { "1.0~1.6" }
        $nLab = if ($noise -lt 2) { "0~2" } elseif ($noise -lt 4) { "2~4" } else { "4~6" }

        Add-Bucket "scale"    $sLab $ok
        Add-Bucket "angle"    $aLab $ok
        Add-Bucket "fillGray" $gLab $ok
        Add-Bucket "blur"     $bLab $ok
        Add-Bucket "noise"    $nLab $ok
    }

    if ($allOk) { $cardExact++ }
}

# ── 报告 ──
Write-Host ""
Write-Host ("=" * 66) -ForegroundColor DarkGray
Write-Host "答题卡压力测试   [N=$($cases.Count), seed=$Seed]" -ForegroundColor Cyan
Write-Host ("=" * 66) -ForegroundColor DarkGray

$acc = if ($qTotal) { $qCorrect / $qTotal } else { 0 }
$cardAcc = if ($cardTotal) { $cardExact / $cardTotal } else { 0 }

Write-Host ""
Write-Host ("  逐题准确率   {0,6:P2}   ({1} / {2})" -f $acc, $qCorrect, $qTotal) -ForegroundColor $(if ($acc -ge $MinAccuracy) { "Green" } else { "Yellow" })
Write-Host ("  整卡全对率   {0,6:P2}   ({1} / {2})" -f $cardAcc, $cardExact, $cardTotal)
Write-Host ("  定位失败     {0,6}     张" -f $notFound)

if ($qBlankTruth -gt 0) {
    Write-Host ""
    Write-Host ("  空白题识别   {0,6:P2}   ({1} / {2})" -f ($qBlankRight / $qBlankTruth), $qBlankRight, $qBlankTruth) -ForegroundColor Cyan
    Write-Host ("  空白误判成选项  {0} 次" -f $qWrongNotBlank)
}

Write-Host ""
Write-Host "  分维度退化曲线（逐题准确率）：" -ForegroundColor DarkGray
foreach ($dim in @("scale", "angle", "fillGray", "blur", "noise")) {
    if (-not $buckets.ContainsKey($dim)) { continue }
    $line = "    {0,-9}" -f $dim
    foreach ($k in ($buckets[$dim].Keys | Sort-Object)) {
        $b = $buckets[$dim][$k]
        $r = if ($b.n) { $b.correct / $b.n } else { 0 }
        $line += ("  {0}={1:P0}({2})" -f $k, $r, $b.n)
    }
    Write-Host $line
}

Write-Host ""
if ($acc -ge $MinAccuracy) {
    Write-Host ("  达标（>= {0:P0}）" -f $MinAccuracy) -ForegroundColor Green
    exit 0
} else {
    Write-Host ("  未达标（< {0:P0}）" -f $MinAccuracy) -ForegroundColor Yellow
    exit 1
}
