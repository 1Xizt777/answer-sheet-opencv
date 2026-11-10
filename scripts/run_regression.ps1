# 答题卡识别回归测试
#
# 用法：
#   .\scripts\run_regression.ps1              # 全部样本
#   .\scripts\run_regression.ps1 -Verbose     # 带详细输出
#   .\scripts\run_regression.ps1 -Baseline    # 基线模式：只报告是否符合当前基线
#
# 退出码：0 = 全部符合预期；1 = 有失败

[CmdletBinding()]
param(
    [switch]$Baseline,          # 基线模式：比对 baseline 字段而非期望
    [switch]$UpdateBaseline,    # 用当前实际输出刷新 baseline 字段
    [string]$Exe = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$samplesDir = Join-Path $root "data\samples"
$debugDir = Join-Path $root "data\debug"

if (-not $Exe) {
    $Exe = Join-Path $root "x64\Debug\answersheet_cli.exe"
}
if (-not (Test-Path $Exe)) {
    Write-Host "ERROR: CLI not found: $Exe" -ForegroundColor Red
    Write-Host "       build it first (see README)" -ForegroundColor Yellow
    exit 1
}

# OpenCV DLL 已由 build.ps1 复制到输出目录，正常情况下无需改 PATH。
# 若你把 exe 挪到别处，可用环境变量 OPENCV_ROOT 指定 OpenCV build 目录。
if ($env:OPENCV_ROOT) {
    $opencvBin = Join-Path $env:OPENCV_ROOT "x64\vc16\bin"
    if (Test-Path $opencvBin) { $env:PATH = "$opencvBin;$env:PATH" }
}

New-Item -ItemType Directory -Force -Path $debugDir | Out-Null

$pass = 0
$fail = 0
$details = @()

$cases = Get-ChildItem $samplesDir -Filter "*.expected.json" | Sort-Object Name

foreach ($case in $cases) {
    # 用 .NET API 读，不用 Get-Content：PS 5.1 的 Get-Content -Encoding UTF8
    # 在无 BOM 文件上会按 ANSI 解析，导致中文 note 乱码
    $spec = [System.IO.File]::ReadAllText($case.FullName, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    $imgPath = Join-Path $samplesDir $spec.image

    if (-not (Test-Path $imgPath)) {
        $details += "SKIP  $($spec.image)  (image missing)"
        continue
    }

    $outJson = Join-Path $env:TEMP "as_$($spec.image -replace '\W','_').json"
    Remove-Item $outJson -ErrorAction SilentlyContinue

    # 运行，抑制 OpenCV INFO 日志
    $raw = & $Exe --image $imgPath --out $outJson --debug-dir (Join-Path $debugDir $spec.image.Replace('.png','')) 2>&1 |
           Where-Object { $_ -notmatch 'INFO:|\bFAILED\b' }
    $exitCode = $LASTEXITCODE

    if (-not (Test-Path $outJson)) {
        $fail++
        $details += "FAIL  $($spec.image)  (no output json, exit=$exitCode)"
        continue
    }

    $actual = Get-Content $outJson -Raw -Encoding UTF8 | ConvertFrom-Json

    # ── 基线模式：只确认行为没变 ──
    if ($Baseline -or $UpdateBaseline) {
        $hasBaseline = $null -ne $spec.PSObject.Properties['baseline']

        if ($UpdateBaseline) {
            if (-not $hasBaseline) {
                $spec | Add-Member -NotePropertyName baseline -NotePropertyValue ([pscustomobject]@{}) -Force
            }
            $spec.baseline | Add-Member -NotePropertyName found   -NotePropertyValue ([bool]$actual.found) -Force
            $spec.baseline | Add-Member -NotePropertyName answers -NotePropertyValue @($actual.answers) -Force

            # 手写紧凑 JSON：这些文件要人读、要看 diff，ConvertTo-Json 会摊成几十行
            $keyStr = ($spec.key   | ForEach-Object { "`"$_`"" }) -join ", "
            $ansStr = ($spec.answers | ForEach-Object {
                            if ($null -eq $_) { "null" } else { "`"$_`"" } }) -join ", "
            $baseAnsStr = if (@($spec.baseline.answers).Count -gt 0) {
                              (@($spec.baseline.answers) | ForEach-Object { "`"$_`"" }) -join ", "
                          } else { "" }

            $sb = [System.Text.StringBuilder]::new()
            [void]$sb.AppendLine("{")
            [void]$sb.AppendLine("  `"image`": `"$($spec.image)`",")
            [void]$sb.AppendLine("  `"expect`": `"$($spec.expect)`",")
            [void]$sb.AppendLine("  `"key`": [$keyStr],")
            [void]$sb.AppendLine("  `"answers`": [$ansStr],")
            [void]$sb.AppendLine("  `"baseline`": {")
            [void]$sb.AppendLine("    `"found`": $(([bool]$actual.found).ToString().ToLower()),")
            [void]$sb.AppendLine("    `"answers`": [$baseAnsStr],")
            $note = if ($null -ne $spec.baseline.PSObject.Properties['note']) { $spec.baseline.note } else { "" }
            [void]$sb.AppendLine("    `"note`": `"$note`"")
            [void]$sb.AppendLine("  }")
            [void]$sb.AppendLine("}")

            [System.IO.File]::WriteAllText($case.FullName, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))
            $details += "UPDT  $($spec.image)  baseline <- $(@($actual.answers) -join '')"
            $pass++
            continue
        }

        if (-not $hasBaseline) {
            $fail++
            $details += "FAIL  $($spec.image)  缺少 baseline 段（跑 -UpdateBaseline 生成）"
            continue
        }

        $baseAns = @($spec.baseline.answers)
        $actAns = @($actual.answers)
        $same = ($spec.baseline.found -eq $actual.found) -and
                (($baseAns -join ',') -eq ($actAns -join ','))

        if ($same) {
            $pass++
            Write-Verbose "OK    $($spec.image)"
        }
        else {
            $fail++
            $details += "FAIL  $($spec.image)  行为变了：基线=$($baseAns -join '') 实际=$($actAns -join '')"
        }
        continue
    }

    # ── 期望模式：对照真实期望 ──
    switch ($spec.expect) {
        "not_found" {
            if (-not $actual.found) { $pass++; Write-Verbose "OK    $($spec.image)" }
            else {
                $fail++
                $details += "FAIL  $($spec.image)  期望找不到卡，实际找到了：$($actual.answers -join '')"
            }
        }
        default {
            if (-not $actual.found) {
                # 已知基线缺陷：如实报告为 UNFIXED，不计入通过
                $fail++
                $details += "FAIL  $($spec.image)  未找到答题卡（已知基线缺陷）"
                break
            }

            $exp = @($spec.answers)
            $act = @($actual.answers)
            $mismatch = @()
            for ($i = 0; $i -lt $exp.Count; $i++) {
                $e = if ($exp[$i]) { $exp[$i] } else { "?" }
                $a = if ($i -lt $act.Count) { $act[$i] } else { "-" }
                if ($e -ne $a) { $mismatch += "Q$($i+1): 期望 $e 实际 $a" }
            }

            if ($mismatch.Count -eq 0) { $pass++; Write-Verbose "OK    $($spec.image)" }
            else {
                $fail++
                $details += "FAIL  $($spec.image)  " + ($mismatch -join "; ")
            }
        }
    }
}

# ── 汇总 ──
Write-Host ""
Write-Host ("=" * 66) -ForegroundColor DarkGray
$mode = if ($Baseline) { "基线模式" } elseif ($UpdateBaseline) { "更新基线" } else { "期望模式" }
Write-Host "答题卡回归测试   [$mode]" -ForegroundColor Cyan
Write-Host ("=" * 66) -ForegroundColor DarkGray

if ($details.Count -gt 0) {
    Write-Host ""
    foreach ($d in $details) {
        $color = if ($d -match '^FAIL') { "Red" } elseif ($d -match '^UPDT') { "Yellow" } else { "DarkGray" }
        Write-Host "  $d" -ForegroundColor $color
    }
}

Write-Host ""
$total = $pass + $fail
$color = if ($fail -eq 0) { "Green" } else { "Yellow" }
Write-Host "  通过 $pass / $total" -ForegroundColor $color

if ($fail -gt 0) {
    Write-Host ""
    Write-Host "  有 $fail 个失败。若是已知基线缺陷，见 data/BASELINE.md" -ForegroundColor Yellow
    exit 1
}

Write-Host "  全部通过" -ForegroundColor Green
exit 0
