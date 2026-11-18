# Structural checks for the whole repository; any finding fails.
#   ./tools/lint.ps1                     text, native rules, ratchets, clang-format (the whole native tree)
#   ./tools/lint.ps1 -Tidy               also clang-tidy (.clang-tidy; a Ninja build in native/build/tidy)
#   ./tools/lint.ps1 -DotNet             also the C# projects, built with their style and quality rules
#   ./tools/lint.ps1 -Fix                format first (clang-format, dotnet format)
#   ./tools/lint.ps1 -Base origin/main   only the changed files for clang-tidy; -MaxLines budget
#   -Paths <globs>                       the change may touch only these, and answers only for them
#   ./tools/lint.ps1 -UpdateBaseline     record the ratchets (only ever lower them)
#   -Root <tree>                         check another checkout with these rules
param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot),
    [string]$Base,
    [string[]]$Paths,
    [int]$MaxLines = 0,
    [switch]$Tidy,
    [switch]$DotNet,
    [switch]$Fix,
    [switch]$UpdateBaseline
)
$ErrorActionPreference = 'Stop'
$tools = $PSScriptRoot
$core = Join-Path $Root 'native/core'
$baselinePath = Join-Path $tools 'lint-baseline.json'
$errors = [System.Collections.Generic.List[string]]::new()
$counts = [ordered]@{}
$Paths = @($Paths | ForEach-Object { $_ -split ',' } | Where-Object { $_ })

function Rel([string]$path) { [IO.Path]::GetRelativePath($Root, $path).Replace('\', '/') }
function Fail([string]$where, [string]$rule, [string]$why) { $errors.Add("${where}: ${rule}: $why") }
function Llvm([string]$exe) {
    $found = (Get-Command $exe -ErrorAction SilentlyContinue).Source
    if ($found) { return $found }
    $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath 2>$null
    @("$vs\VC\Tools\Llvm\x64\bin\$exe", "D:\Microsoft\VC\Tools\Llvm\x64\bin\$exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
}

$files = @(git -C $Root ls-files --cached --others --exclude-standard | Where-Object { Test-Path (Join-Path $Root $_) })
$native = @($files | Where-Object { $_ -match '^native/(core|tests)/.*\.(cpp|h|hlsl)$' })
$projects = @($files | Where-Object { $_ -match '\.csproj$' } | ForEach-Object { Join-Path $Root $_ })
$clangFormat = Llvm 'clang-format.exe'
$style = Join-Path $tools '..\.clang-format'
$sources = @($native | ForEach-Object { Join-Path $Root $_ })
if ($Fix) {
    if ($clangFormat) { & $clangFormat -i "--style=file:$style" @sources }
    if ($DotNet) { $projects | ForEach-Object { dotnet format $_ | Out-Null } }
}

# --- Text: every source and configuration file ---------------------------------------------------------------
$text = '\.(cpp|h|hlsl|cs|xaml|ps1|csproj|props|json|yml|md|cmake|txt|manifest|rc)$|(^|/)(CMakeLists\.txt|\.[a-z-]+)$'
$code = '\.(cpp|h|hlsl|cs|xaml|ps1)$'
foreach ($rel in $files | Where-Object { $_ -match $text -and $_ -notmatch '^tests/fixtures/' }) {
    $bytes = [IO.File]::ReadAllBytes((Join-Path $Root $rel))
    if (-not $bytes.Length) { continue }
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF -and $rel -notmatch '\.ps1$') {
        Fail $rel 'bom' 'UTF-8 without a byte order mark'
    }
    if ($bytes[-1] -ne 10) { Fail $rel 'final-newline' 'end the file with a newline' }
    $content = [Text.Encoding]::UTF8.GetString($bytes)
    $crlf = $rel -match '\.(ps1|sln)$'
    $lines = $content.TrimEnd("`n").Split("`n")
    $blank = 0
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        $where = "${rel}:$($i + 1)"
        if ($line.EndsWith("`r")) {
            if (-not $crlf) { Fail $where 'line-ending' 'LF only' }
            $line = $line.Substring(0, $line.Length - 1)
        } elseif ($crlf) { Fail $where 'line-ending' 'CRLF for PowerShell' }
        if ($line.Contains("`t")) { Fail $where 'tab' 'indent with spaces' }
        if ($line -match '\s$' -and $rel -notmatch '\.md$') { Fail $where 'trailing-space' 'delete it' }
        # A line of one XAML attribute or one string (a sentence to translate) cannot be wrapped further.
        $unbreakable = ($rel.EndsWith('.xaml') -and $line -match '^\s*[\w:.]+="[^"]*"\s*/?>?\s*$') -or
            ($rel -match '\.(cs|cpp|h)$' -and $line -match '^\s*((Loc\.[TF]|Tr|TrFormat)\()?L?"(?:[^"\\]|\\.)*"\)?[,;)]*\s*$')
        if ($rel -match $code -and $line.Length -gt 160 -and -not $unbreakable) { Fail $where 'line-length' "$($line.Length) > 160 columns" }
        $blank = if ($line.Trim()) { 0 } else { $blank + 1 }
        if ($blank -eq 2 -and $rel -match $code) { Fail $where 'blank-lines' 'one blank line at most' }
    }
}

# --- XAML: an element sits four spaces deeper than its parent; a start tag's other lines follow it ------------
foreach ($rel in $files | Where-Object { $_ -match '\.xaml$' }) {
    $lines = [IO.File]::ReadAllLines((Join-Path $Root $rel))
    $depth = 0
    $open = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $s = $lines[$i].Trim()
        if ($open) {
            if ($s.EndsWith('/>') -or $s -match '</[\w:.]+>$') { $open = $false } elseif ($s.EndsWith('>')) { $open = $false; $depth++ }
            continue
        }
        if (-not $s.StartsWith('<') -or $s.StartsWith('<?')) { continue }
        if ($s.StartsWith('</')) { $depth-- }
        $indent = $lines[$i].Length - $lines[$i].TrimStart(' ').Length
        if ($indent -ne 4 * $depth) { Fail "${rel}:$($i + 1)" 'xaml-indent' "indent $(4 * $depth) spaces, the element's depth" }
        if ($s.StartsWith('</') -or $s.StartsWith('<!--') -or $s.EndsWith('/>') -or $s -match '</[\w:.]+>$') { continue }
        if ($s.EndsWith('>')) { $depth++ } else { $open = $true }
    }
}

# --- Native: layers, banned and ratcheted constructs ----------------------------------------------------------
# Who may include whom. Root headers are either foundation (anyone) or composition (root only).
$foundation = 'bar_edge.h', 'clock.h', 'icon_image.h', 'localization.h', 'localization_format.h', 'log.h', 'motion.h', 'notice.h', 'posted.h', 'resource.h',
    'settings.h', 'status.h'
$layers = @{ render = @(); shell = @(); ui = @('render', 'shell') }
# Configuration macros set before the headers that read them; any other value is a constexpr.
$configMacros = 'SECURITY_WIN32', 'WIN32_LEAN_AND_MEAN', 'NOMINMAX', 'IDI_ATRIUM'
$banned = [ordered]@{
    'null-macro'    = @('\bNULL\b', 'use nullptr')
    'todo'          = @('\b(TODO|FIXME|XXX|HACK)\b', 'open an issue instead')
    'endl'          = @('std::endl', "use '\n'")
    'assert'        = @('\bassert\s*\(', 'handle the case or let it crash in a test')
    'using-std'     = @('using\s+namespace\s+std\b', 'qualify std::')
    'include-guard' = @('^#ifndef\s+\w+_H\w*\s*$', 'use #pragma once')
    'print'         = @('\b(w?printf|std::w?cout|std::w?cerr|OutputDebugString\w*)\s*[(<]', 'use Log (tests print their results)')
    'nolint'        = @('NOLINT(NEXTLINE|BEGIN|END)?(?!\([\w.,*-]+\): \S)', 'name the check and the reason: NOLINT(check): why')
    'narration'     = @(('//\s*(Check|Get|Set|Call|Loop|Iterate|Increment|Decrement|Initialize|Create|Return|Update|Add|Remove|Handle)' +
        '\s+(if|the|a|an|whether|over|through)\b'), 'say why, not what the next line does')
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
$fileLines = 800 # a file longer than this is split by what it does

foreach ($rel in $native) {
    $path = Join-Path $Root $rel
    $lines = [IO.File]::ReadAllLines($path)
    $coreRel = if ($rel.StartsWith('native/core/')) { $rel.Substring(12) } else { '' }
    $from = if ($coreRel.Contains('/')) { $coreRel.Split('/')[0] } else { '' }
    if ($rel -match '\.h$' -and $lines.Count -and $lines[0] -notmatch '^// ') { Fail "${rel}:1" 'header-comment' 'begin with what the header is for' }
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        $where = "${rel}:$($i + 1)"
        foreach ($rule in $banned.Keys) {
            if ($line -cmatch $banned[$rule][0] -and -not ($rule -eq 'print' -and ($rel -match '^native/tests/' -or $coreRel -eq 'log.cpp'))) {
                Fail $where $rule $banned[$rule][1]
            }
        }
        if ($line -match '^\s*using\s+namespace\b' -and $rel -match '\.h$') { Fail $where 'using-in-header' 'qualify names in headers' }
        $guarded = $i -and $lines[$i - 1] -match '^\s*#ifndef\s+(\w+)' -and $line -match "^\s*#define\s+$($Matches[1])\b"
        if ($line -match '^\s*#define\s+(\w+)' -and $configMacros -notcontains $Matches[1] -and -not $guarded) {
            Fail $where 'define' 'use constexpr (a macro only to supply what an older SDK lacks, under #ifndef)'
        }
        if ($from -and $line -match '^\s*#include\s+"([^"]+)"') {
            $target = [IO.Path]::GetFullPath((Join-Path (Split-Path $path) $Matches[1]))
            if (-not (Test-Path $target)) { $target = [IO.Path]::GetFullPath((Join-Path $core $Matches[1])) }
            if (-not (Test-Path $target)) { continue }
            $to = [IO.Path]::GetRelativePath($core, $target).Replace('\', '/')
            $toLayer = if ($to.Contains('/')) { $to.Split('/')[0] } else { '' }
            $allowed = if ($toLayer) { $toLayer -eq $from -or $layers[$from] -contains $toLayer } else { $foundation -contains $to }
            if (-not $allowed) { Fail $where 'layer' "$from/ may not include $to" }
        }
    }
    $key = $rel.Substring(7)
    foreach ($rule in $ratcheted.Keys) {
        $n = @($lines | Where-Object { $_ -cmatch $ratcheted[$rule][0] }).Count
        if ($n) { $counts["$key|$rule"] = $n }
    }
    if ($lines.Count -gt $fileLines) { $counts["$key|file-lines"] = $lines.Count }
}

# --- One version: the native project, the settings app and both manifests --------------------------------------
$versions = [ordered]@{
    'native/CMakeLists.txt'                        = 'project\(\w+ VERSION (\d+\.\d+\.\d+)'
    'settings/Atrium.Settings/Atrium.Settings.csproj' = '<Version>(\d+\.\d+\.\d+)</Version>'
    'native/core/core.manifest'                    = '<assemblyIdentity type="win32" name="Atrium" version="(\d+\.\d+\.\d+)\.0"'
    'settings/Atrium.Settings/app.manifest'        = '<assemblyIdentity version="(\d+\.\d+\.\d+)\.0"'
}
$found = foreach ($file in $versions.Keys) {
    $m = [regex]::Match([IO.File]::ReadAllText((Join-Path $Root $file)), $versions[$file])
    [pscustomobject]@{ File = $file; Version = if ($m.Success) { $m.Groups[1].Value } else { '?' } }
}
$expected = $found[0].Version
foreach ($f in $found | Where-Object Version -ne $expected) { Fail $f.File 'version' "$($f.Version), the project is $expected" }

# --- clang-format: the whole native tree ----------------------------------------------------------------------
if (-not $clangFormat) { $errors.Add('lint: clang-format not found') }
else {
    foreach ($m in & $clangFormat --dry-run "--style=file:$style" @sources 2>&1 | Select-String '^(.+?):(\d+):\d+: warning') {
        Fail "$(Rel $m.Matches[0].Groups[1].Value):$($m.Matches[0].Groups[2].Value)" 'format' 'run ./tools/lint.ps1 -Fix'
    }
}

# --- clang-tidy: .clang-tidy over a Ninja build of the native tree --------------------------------------------
if ($Tidy) {
    $build = Join-Path $Root 'native/build/tidy'
    $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath 2>$null
    if (-not $vs) { $vs = 'D:\Microsoft' }
    foreach ($line in cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && set") {
        if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2]) }
    }
    cmake -S (Join-Path $Root 'native') -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON `
        -DATRIUM_DEVELOPMENT=ON -DATRIUM_BUILD_TESTS=ON | Out-Null
    cmake --build $build | Out-Null # the generated shader headers
    if ($LASTEXITCODE) { $errors.Add('lint: the tidy build failed (cmake --build native/build/tidy)') }
    $units = @((Get-Content (Join-Path $build 'compile_commands.json') -Raw | ConvertFrom-Json).file |
        Where-Object { $_ -match '[\\/]native[\\/](core|tests)[\\/]' } | Sort-Object -Unique)
    if ($Base) {
        $touched = @(git -C $Root diff --name-only $Base -- native | ForEach-Object { [IO.Path]::GetFileName($_) })
        $headers = @($touched | Where-Object { $_ -match '\.h$' })
        $units = @($units | Where-Object {
            $touched -contains [IO.Path]::GetFileName($_) -or
            ($headers -and (Select-String -Path $_ -Pattern ($headers | ForEach-Object { [regex]::Escape($_) + '"' }) -Quiet))
        })
    }
    $clangTidy = Llvm 'clang-tidy.exe'
    $output = $units | ForEach-Object -ThrottleLimit ([Environment]::ProcessorCount) -Parallel {
        & $using:clangTidy -p $using:build --quiet $_ 2>$null
    }
    $seen = @{}
    $current = $null
    foreach ($line in $output) {
        if ($line -match '^(.+?):(\d+):\d+: (?:warning|error): (.*) \[([\w.,-]+)\]$') {
            $file, $row, $text, $check = $Matches[1..4]
            $rel = Rel $file
            $current = $null
            if (-not $rel.StartsWith('native/core/') -and -not $rel.StartsWith('native/tests/')) { continue }
            $where = "${rel}:$row"
            if ($seen.ContainsKey("$where|$check")) { continue }
            $seen["$where|$check"] = $true
            # Long and tangled functions are ratcheted: they may only get shorter and simpler.
            if ($check -eq 'readability-function-cognitive-complexity' -and $text -match "function '(.+)' has cognitive complexity of (\d+)") {
                $counts["$($rel.Substring(7))|complexity|$($Matches[1])"] = [int]$Matches[2]
            } elseif ($check -eq 'readability-function-cognitive-complexity' -and $text -match 'lambda has cognitive complexity of (\d+)') {
                $key = "$($rel.Substring(7))|complexity|(lambda)" # the file's most tangled one
                $counts[$key] = [Math]::Max([int]$counts[$key], [int]$Matches[1])
            } elseif ($check -eq 'readability-function-size' -and $text -match "function '(.+)' exceeds") {
                $current = "$($rel.Substring(7))|function-lines|$($Matches[1])"
                $counts[$current] = 0
            } else {
                Fail $where $check $text
            }
        } elseif ($current -and $line -match 'note: (\d+) lines including') {
            $counts[$current] = [int]$Matches[1]
        }
    }
}

# --- C#: the projects built with .editorconfig's style rules and the recommended analyzers as errors ----------
if ($DotNet) {
    foreach ($project in $projects) {
        foreach ($m in dotnet build $project -c Release -nologo 2>&1 | Select-String '^(.+?\.(?:cs|xaml))\((\d+),\d+\): error (\w+): (.*?)( \[.*)?$') {
            $g = $m.Matches[0].Groups
            Fail "$(Rel $g[1].Value):$($g[2].Value)" $g[3].Value $g[4].Value
        }
    }
}

# --- Ratchets ---------------------------------------------------------------------------------------------------
if ($UpdateBaseline) {
    $sorted = [ordered]@{}
    foreach ($key in $counts.Keys | Sort-Object) { $sorted[$key] = $counts[$key] }
    [IO.File]::WriteAllText($baselinePath, ($sorted | ConvertTo-Json).Replace("`r`n", "`n") + "`n")
    "baseline: $($counts.Count) entries"
    exit 0
}
$baseline = if (Test-Path $baselinePath) { Get-Content $baselinePath -Raw | ConvertFrom-Json -AsHashtable } else { @{} }
$advice = @{ 'file-lines' = "split it by what it does (at most $fileLines lines)"; 'complexity' = 'simplify it (cognitive complexity 40 at most)'
    'function-lines' = 'split it (120 lines at most)' }
foreach ($key in $counts.Keys) {
    $limit = if ($baseline.ContainsKey($key)) { $baseline[$key] } else { 0 }
    if ($counts[$key] -gt $limit) {
        $parts = $key.Split('|')
        $why = if ($ratcheted.Contains($parts[1])) { $ratcheted[$parts[1]][1] } else { $advice[$parts[1]] }
        Fail "native/$($parts[0])" "$($parts[1..($parts.Count - 1)] -join ' ')" "$($counts[$key]) > $limit; $why"
    }
}
foreach ($key in $baseline.Keys) {
    $tidyKey = $key -match '\|(complexity|function-lines)\|'
    if (($tidyKey -and -not $Tidy) -or ($Base -and $tidyKey)) { continue }
    if (-not $counts.Contains($key) -or $counts[$key] -lt $baseline[$key]) {
        $errors.Add("lint: $key is below its baseline; run ./tools/lint.ps1 -Tidy -UpdateBaseline")
    }
}

# --- The change itself --------------------------------------------------------------------------------------------
if ($Base) {
    $changed = @(git -C $Root diff --name-only --diff-filter=AMR $Base -- .)
    foreach ($path in $changed) {
        if ($Paths -and -not ($Paths | Where-Object { $path -like $_ })) { Fail $path 'scope' "outside the allowed paths ($($Paths -join ', '))" }
    }
    $numbers = git -C $Root diff --numstat $Base -- . | ForEach-Object { ($_ -split "`t")[0..1] -replace '-', '0' }
    $total = ($numbers | ForEach-Object { [int]$_ } | Measure-Object -Sum).Sum
    if ($MaxLines -and $total -gt $MaxLines) { $errors.Add("diff: $total changed lines > budget $MaxLines") }
}

$problems = @($errors | Sort-Object -Unique)
# A scoped change answers for its own files (and its diff), not for the rest of the tree.
if ($Paths) {
    $problems = @($problems | Where-Object {
        $file = if ($_ -match '^([^:]+?)(:\d+)?: ') { $Matches[1] } else { '' }
        $_ -match '^diff: |: scope: ' -or ($Paths | Where-Object { $file -like $_ })
    })
}
$problems | ForEach-Object { Write-Output $_ }
if ($problems.Count) { Write-Output "lint: $($problems.Count) problem(s)"; exit 1 }
Write-Output 'lint: clean'
