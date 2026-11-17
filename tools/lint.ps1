# Structural checks for native code: layer boundaries, banned constructs, ratcheted
# constructs and, against a base revision, formatting of the changed lines.
#   ./tools/lint.ps1                         whole tree
#   ./tools/lint.ps1 -Base origin/main       also the diff: format, -Paths scope, -MaxLines budget
#   ./tools/lint.ps1 -UpdateBaseline         record the ratchet counts (only ever lower them)
#   -Root <tree>                             check another checkout with these rules
#   -Fix                                     with -Base: format the changed lines in place first
param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot),
    [string]$Base,
    [string[]]$Paths,
    [int]$MaxLines = 0,
    [switch]$Fix,
    [switch]$UpdateBaseline
)
$ErrorActionPreference = 'Stop'
$root = $Root
$style = Join-Path (Split-Path -Parent $PSScriptRoot) .clang-format
$core = Join-Path $root 'native/core'
$baselinePath = Join-Path $PSScriptRoot 'lint-baseline.json'
$errors = [System.Collections.Generic.List[string]]::new()
$Paths = @($Paths | ForEach-Object { $_ -split ',' } | Where-Object { $_ })

# Who may include whom. Root headers are either foundation (anyone) or composition (root only).
$foundation = 'bar_edge.h', 'clock.h', 'localization.h', 'localization_format.h', 'log.h', 'motion.h', 'resource.h', 'settings.h'
$layers = @{ render = @(); shell = @(); ui = @('render', 'shell') }
$layerExceptions = @(
    'render/bar_painter.h -> shell/system_status.h'
    'render/bar_painter.h -> shell/task_model.h'
    'shell/feishu_source.h -> ui/notifications.h'
    'shell/notification_listener.h -> ui/notifications.h'
    'shell/update_checker.cpp -> ui/notifications.h'
    'shell/wechat_source.h -> ui/notifications.h'
)

$banned = [ordered]@{
    'null-macro'      = @('\bNULL\b', 'use nullptr')
    'todo'            = @('\b(TODO|FIXME|XXX|HACK)\b', 'open an issue instead')
    'endl'            = @('std::endl', "use '\n'")
    'assert'          = @('\bassert\s*\(', 'handle the case or let it crash in a test')
    'using-std'       = @('using\s+namespace\s+std\b', 'qualify std::')
    'include-guard'   = @('^#ifndef\s+\w+_H\w*\s*$', 'use #pragma once')
    'narration'       = @('//\s*(Check|Get|Set|Call|Loop|Iterate|Increment|Decrement|Initialize|Create|Return|Update|Add|Remove|Handle)\s+(if|the|a|an|whether|over|through)\b', 'say why, not what the next line does')
}
# Counted per file; a file may never hold more than its baseline.
$ratcheted = [ordered]@{
    'catch-all'      = @('catch\s*\(\.\.\.\)', 'catch what can be thrown, at the boundary that can act on it')
    'try'            = @('\btry\s*\{', 'exceptions belong at API boundaries (WinRT, std::stoi), not around internal calls')
    'sleep'          = @('\bSleep\s*\(', 'wait on an event or a timer')
    'raw-new'        = @('(=|return)\s*new\s', 'own it with std::unique_ptr / winrt::com_ptr')
    'raw-delete'     = @('\bdelete\s+[\w\[]', 'own it with std::unique_ptr / winrt::com_ptr')
    'goto'           = @('\bgoto\b', 'structure the loop')
    'commented-code' = @('^\s*//\s*([\w:.<>\[\]]+(->\w+)*\s*(\(.*\)|[+\-*/]?=[^=].*)|return\b.*|[{}])\s*;?\s*$', 'delete it; git keeps history')
}

function Rel([string]$path) { [IO.Path]::GetRelativePath($core, $path).Replace('\', '/') }
function Layer([string]$rel) { if ($rel.Contains('/')) { $rel.Split('/')[0] } else { '' } }

$files = Get-ChildItem $core -Recurse -File -Include *.cpp, *.h, *.hlsl | Where-Object { $_.FullName -notmatch '[\\/]build[\\/]' }
$counts = [ordered]@{}
foreach ($file in $files) {
    $rel = Rel $file.FullName
    $lines = [IO.File]::ReadAllLines($file.FullName)
    $from = Layer $rel
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        foreach ($rule in $banned.Keys) {
            if ($line -cmatch $banned[$rule][0]) { $errors.Add("native/core/${rel}:$($i + 1): $rule`: $($banned[$rule][1])") }
        }
        if ($from -and $line -match '^\s*#include\s+"([^"]+)"') {
            $target = [IO.Path]::GetFullPath((Join-Path $file.DirectoryName $Matches[1]))
            if (-not (Test-Path $target)) { $target = [IO.Path]::GetFullPath((Join-Path $core $Matches[1])) }
            if (-not (Test-Path $target)) { continue }
            $to = Rel $target
            $toLayer = Layer $to
            $allowed = if ($toLayer) { $toLayer -eq $from -or $layers[$from] -contains $toLayer } else { $foundation -contains $to }
            if (-not $allowed -and $layerExceptions -notcontains "$rel -> $to") {
                $errors.Add("native/core/${rel}:$($i + 1): layer: $from/ may not include $to")
            }
        }
    }
    foreach ($rule in $ratcheted.Keys) {
        $n = @($lines | Where-Object { $_ -cmatch $ratcheted[$rule][0] }).Count
        if ($n) { $counts["$rel|$rule"] = $n }
    }
}

if ($UpdateBaseline) {
    $sorted = [ordered]@{}
    foreach ($key in $counts.Keys | Sort-Object) { $sorted[$key] = $counts[$key] }
    [IO.File]::WriteAllText($baselinePath, ($sorted | ConvertTo-Json).Replace("`r`n", "`n") + "`n")
    "baseline: $($counts.Count) entries"
    exit 0
}
$baseline = if (Test-Path $baselinePath) { Get-Content $baselinePath -Raw | ConvertFrom-Json -AsHashtable } else { @{} }
foreach ($key in $counts.Keys) {
    $limit = if ($baseline.ContainsKey($key)) { $baseline[$key] } else { 0 }
    if ($counts[$key] -gt $limit) {
        $rel, $rule = $key.Split('|')
        $errors.Add("native/core/${rel}: $rule`: $($counts[$key]) > $limit; $($ratcheted[$rule][1])")
    }
}
foreach ($key in $baseline.Keys) {
    if (-not $counts.Contains($key) -or $counts[$key] -lt $baseline[$key]) { $errors.Add("lint: $key is below its baseline; run ./tools/lint.ps1 -UpdateBaseline") }
}
foreach ($key in $layerExceptions) {
    $rel, $to = $key -split ' -> '
    if (-not (Select-String -Quiet -SimpleMatch "/$(Split-Path -Leaf $to)`"" (Join-Path $core $rel))) { $errors.Add("lint: stale layer exception '$key'; remove it") }
}

if ($Base) {
    $changed = @(git -C $root diff --name-only --diff-filter=AMR $Base -- .)
    if ($Paths) {
        foreach ($path in $changed) {
            if (-not ($Paths | Where-Object { $path -like $_ })) { $errors.Add("${path}: scope: outside the allowed paths ($($Paths -join ', '))") }
        }
    }
    $stat = git -C $root diff --numstat $Base -- . | ForEach-Object { $a, $d, $null = $_ -split "`t"; [int]($a -replace '-', '0') + [int]($d -replace '-', '0') }
    $total = ($stat | Measure-Object -Sum).Sum
    if ($MaxLines -and $total -gt $MaxLines) { $errors.Add("diff: $total changed lines > budget $MaxLines") }
    $clangFormat = (Get-Command clang-format -ErrorAction SilentlyContinue).Source
    if (-not $clangFormat) {
        $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath 2>$null
        $clangFormat = @("$vs\VC\Tools\Llvm\x64\bin\clang-format.exe", 'D:\Microsoft\VC\Tools\Llvm\x64\bin\clang-format.exe') | Where-Object { Test-Path $_ } | Select-Object -First 1
    }
    if (-not $clangFormat) { $errors.Add('lint: clang-format not found') }
    foreach ($path in $changed | Where-Object { $_ -match '\.(cpp|h)$' -and $_ -like 'native/*' }) {
        $touched = [System.Collections.Generic.HashSet[int]]::new()
        $ranges = @(git -C $root diff -U0 $Base -- $path | Select-String '^@@ .* \+(\d+)(?:,(\d+))? @@' | ForEach-Object {
            $start = [int]$_.Matches[0].Groups[1].Value
            $count = if ($_.Matches[0].Groups[2].Success) { [int]$_.Matches[0].Groups[2].Value } else { 1 }
            for ($n = $start; $n -lt $start + $count; $n++) { [void]$touched.Add($n) }
            if ($count) { "--lines=${start}:$($start + $count - 1)" }
        })
        if (-not $ranges -or -not $clangFormat) { continue }
        if ($Fix) { '' | & $clangFormat -i "--style=file:$style" @ranges (Join-Path $root $path) }
        # clang-format still flags some lines outside --lines (nested conditionals): only the touched ones count.
        $out = '' | & $clangFormat --dry-run "--style=file:$style" @ranges (Join-Path $root $path) 2>&1
        foreach ($m in $out | Select-String ':(\d+):\d+: warning') {
            $n = [int]$m.Matches[0].Groups[1].Value
            if ($touched.Contains($n)) { $errors.Add("${path}:${n}: format: run clang-format on the changed lines") }
        }
    }
}

$problems = @($errors | Sort-Object -Unique)
$problems | ForEach-Object { Write-Output $_ }
if ($problems.Count) { Write-Output "lint: $($problems.Count) problem(s)"; exit 1 }
Write-Output 'lint: clean'
