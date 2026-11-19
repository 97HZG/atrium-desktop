# Structural checks for the whole repository; any finding fails.
#   ./tools/lint.ps1                     text, native rules, ratchets, clang-format (the whole native tree)
#   ./tools/lint.ps1 -Tidy               also clang-tidy (.clang-tidy; a Ninja build in native/build/tidy)
#   ./tools/lint.ps1 -DotNet             also the C# projects, built with their style and quality rules
#   ./tools/lint.ps1 -Fix                format first (clang-format, dotnet format)
#   ./tools/lint.ps1 -Base origin/main   only the changed files for clang-tidy; -MaxLines budget
#   -Paths <globs>                       the change may touch only these, and answers only for them
#   ./tools/lint.ps1 -Tidy -UpdateBaseline  record the ratchets (only ever lower them)
#   -Root <tree>                         check another checkout with these rules
param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot),
    [string]$RulesRoot,
    [string]$Base,
    [string[]]$Paths,
    [int]$MaxLines = 0,
    [switch]$Tidy,
    [switch]$DotNet,
    [switch]$Fix,
    [switch]$UpdateBaseline
)
$ErrorActionPreference = 'Stop'
$rules = if ($RulesRoot) { $RulesRoot } else { $Root }
$core = Join-Path $Root 'native/core'
$baselinePath = Join-Path $Root 'tools/lint-baseline.json'
$errors = [System.Collections.Generic.List[string]]::new()
$counts = [ordered]@{}
$Paths = @($Paths | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
if ($UpdateBaseline -and ($Base -or $Paths -or $MaxLines -or -not $Tidy)) {
    throw 'lint: -UpdateBaseline requires -Tidy and cannot be used with -Base, -Paths, or -MaxLines'
}

function Rel([string]$path) { [IO.Path]::GetRelativePath($Root, $path).Replace('\', '/') }
function Fail([string]$where, [string]$rule, [string]$why) { $errors.Add("${where}: ${rule}: $why") }
function Tail([string[]]$lines, [int]$n = 15) {
    if (-not $lines -or -not $lines.Count) { return '(no output)' }
    ($lines | Select-Object -Last $n) -join "`n"
}
function Invoke-Tool([string]$Exe, [string[]]$ToolArgs, [switch]$AllowNonZero) {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $global:LASTEXITCODE = -1
        $lines = @(& $Exe @ToolArgs 2>&1 | ForEach-Object { "$_" })
        $code = $LASTEXITCODE
    } catch {
        throw "lint: $Exe failed: $_"
    } finally {
        $ErrorActionPreference = $prev
    }
    if ($code -ne 0 -and -not $AllowNonZero) {
        throw "lint: $Exe $($ToolArgs -join ' ') failed (exit $code):`n$(Tail $lines)"
    }
    [pscustomobject]@{ ExitCode = $code; Lines = $lines }
}
function Test-Glob([string]$path, [string[]]$globs) {
    $p = $path.Replace('\', '/') -replace '^\./', ''
    foreach ($g in $globs) {
        $rx = '^'
        $i = 0
        $n = $g.Replace('\', '/') -replace '^\./', ''
        while ($i -lt $n.Length) {
            if ($i + 2 -lt $n.Length -and $n.Substring($i, 3) -eq '**/') { $rx += '(?:.+/)?'; $i += 3 }
            elseif ($i + 1 -lt $n.Length -and $n.Substring($i, 2) -eq '**') { $rx += '.*'; $i += 2 }
            elseif ($n[$i] -eq '*') { $rx += '[^/]*'; $i++ }
            elseif ($n[$i] -eq '?') { $rx += '[^/]'; $i++ }
            else { $rx += [regex]::Escape($n[$i]); $i++ }
        }
        if ($p -match "$rx`$") { return $true }
    }
    $false
}
# The LLVM tools of one major version: another clang-tidy brings other checks, another clang-format other layouts.
$llvmMajor = 19
function Llvm([string]$exe) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = if (Test-Path $vswhere) { & $vswhere -latest -products * -property installationPath 2>$null } else { $null }
    $candidates = @((Get-Command $exe -All -ErrorAction SilentlyContinue).Source) +
        $(if ($vs) { @("$vs\VC\Tools\Llvm\x64\bin\$exe") } else { @() })
    $candidates | Where-Object { $_ -and (Test-Path $_) -and (& $_ --version) -match "version $llvmMajor\." } | Select-Object -First 1
}

$diffStatus = @()
$diffNumstat = @()
$touchedFiles = @()
$fullRun = $false
if ($Base) {
    Invoke-Tool 'git' @('-C', $Root, 'rev-parse', '--verify', "$Base^{commit}") | Out-Null
    $mb = Invoke-Tool 'git' @('-C', $Root, 'merge-base', $Base, 'HEAD') -AllowNonZero
    if ($mb.ExitCode -ne 0 -or -not $mb.Lines) {
        throw "lint: git merge-base $Base HEAD failed (shallow clone or no common ancestor): $(Tail $mb.Lines)"
    }
    $mergeBase = @($mb.Lines)[0].Trim()
    $prevIndex = $env:GIT_INDEX_FILE
    $realIndex = if ($prevIndex) { $prevIndex } else {
        @((Invoke-Tool 'git' @('-C', $Root, 'rev-parse', '--path-format=absolute', '--git-path', 'index')).Lines)[0].Trim()
    }
    $tmpIndex = Join-Path ([IO.Path]::GetTempPath()) "atrium-lint-$([Guid]::NewGuid().ToString('N')).index"
    try {
        if (Test-Path $realIndex) { Copy-Item $realIndex $tmpIndex -Force }
        $env:GIT_INDEX_FILE = $tmpIndex
        $untracked = @((Invoke-Tool 'git' @('-C', $Root, '-c', 'core.quotepath=false', 'ls-files', '--others', '--exclude-standard')).Lines)
        if ($untracked) { Invoke-Tool 'git' (@('-C', $Root, 'add', '-N', '--') + $untracked) | Out-Null }
        $diffStatus = @((Invoke-Tool 'git' @('-C', $Root, '-c', 'core.quotepath=false', 'diff', '--name-status', '-M', $mergeBase, '--', '.')).Lines)
        $diffNumstat = @((Invoke-Tool 'git' @('-C', $Root, '-c', 'core.quotepath=false', 'diff', '--numstat', $mergeBase, '--', '.')).Lines)
    } finally {
        $env:GIT_INDEX_FILE = $prevIndex
        Remove-Item $tmpIndex, "$tmpIndex.lock" -ErrorAction SilentlyContinue
    }
    $touchedFiles = @($diffStatus | ForEach-Object {
        $parts = $_ -split "`t"
        if ($parts[0] -like 'R*') { $parts[1], $parts[2] } elseif ($parts[0] -like 'C*') { $parts[2] } else { $parts[1] }
    })
    $fullConfig = '.clang-tidy', '.clang-format', '.editorconfig', '.gitattributes', 'Directory.Build.props', 'global.json',
        'tools/lint.ps1', 'tools/lint-baseline.json', 'tools/check-localization.ps1',
        'native/CMakeLists.txt', 'native/**/CMakeLists.txt', 'native/CMakePresets.json', '.github/workflows/**'
    $fullRun = [bool]($touchedFiles | Where-Object { Test-Glob $_ $fullConfig } | Select-Object -First 1)
    if ($fullRun -and ($touchedFiles | Where-Object {
        Test-Glob $_ @('**/*.cs', '**/*.xaml', '**/*.csproj', '.editorconfig', 'Directory.Build.props', 'global.json')
    } | Select-Object -First 1)) { $DotNet = $true }
}

$files = @((Invoke-Tool 'git' @('-C', $Root, '-c', 'core.quotepath=false', 'ls-files', '--cached', '--others', '--exclude-standard')).Lines |
    Where-Object { Test-Path (Join-Path $Root $_) })
$native = @($files | Where-Object { $_ -match '^native/(core|tests)/.*\.(cpp|h|hlsl)$' })
$projects = @($files | Where-Object { $_ -match '\.csproj$' } | ForEach-Object { Join-Path $Root $_ })
$clangFormat = Llvm 'clang-format.exe'
$style = Join-Path $rules '.clang-format'
$sources = @($native | ForEach-Object { Join-Path $Root $_ })
if ($Fix) {
    if ($clangFormat -and $sources) {
        $rsp = [IO.Path]::GetTempFileName()
        try {
            [IO.File]::WriteAllLines($rsp, $sources)
            Invoke-Tool $clangFormat @('-i', "--style=file:$style", "--files=$rsp") | Out-Null
        } finally {
            Remove-Item $rsp -ErrorAction SilentlyContinue
        }
    }
    if ($DotNet) { $projects | ForEach-Object { Invoke-Tool 'dotnet' @('format', $_) | Out-Null } }
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
    'pinned_app.h', 'settings.h', 'stage_excluded.h', 'status.h', 'win32_path.h'
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
if (-not $clangFormat) { $errors.Add("lint: clang-format $llvmMajor not found") }
elseif ($sources) {
    $rsp = [IO.Path]::GetTempFileName()
    try {
        [IO.File]::WriteAllLines($rsp, $sources)
        $res = Invoke-Tool $clangFormat @('--dry-run', '--Werror', "--style=file:$style", "--files=$rsp") -AllowNonZero
        $hits = 0
        foreach ($line in $res.Lines) {
            if ($line -match '^(.+?):(\d+):\d+: (?:warning|error): code should be clang-formatted') {
                $hits++
                Fail "$(Rel $Matches[1]):$($Matches[2])" 'format' 'run ./tools/lint.ps1 -Fix'
            }
        }
        if ($res.ExitCode -ne 0 -and -not $hits) {
            $errors.Add("tool-error: clang-format failed (exit $($res.ExitCode)): $(Tail $res.Lines)")
        }
    } finally {
        Remove-Item $rsp -ErrorAction SilentlyContinue
    }
}

# --- clang-tidy: .clang-tidy over a Ninja build of the native tree --------------------------------------------
if ($Tidy) {
    $build = Join-Path $Root 'native/build/tidy'
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
        $vs = if (Test-Path $vswhere) { & $vswhere -latest -products * -property installationPath 2>$null } else { $null }
        $vcvars = if ($vs) { Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat' } else { $null }
        if (-not $vcvars -or -not (Test-Path $vcvars)) { throw 'lint: cl.exe not on PATH and Visual Studio vcvars64.bat not found' }
        foreach ($line in (Invoke-Tool 'cmd' @('/c', "`"$vcvars`" >nul && set")).Lines) {
            if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2]) }
        }
    }
    Invoke-Tool 'cmake' @('-S', (Join-Path $Root 'native'), '-B', $build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
        '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', '-DATRIUM_DEVELOPMENT=ON', '-DATRIUM_BUILD_TESTS=ON') | Out-Null
    Invoke-Tool 'cmake' @('--build', $build) | Out-Null # the generated shader headers
    $units = @((Get-Content (Join-Path $build 'compile_commands.json') -Raw | ConvertFrom-Json).file |
        Where-Object { $_ -match '[\\/]native[\\/](core|tests)[\\/].*\.cpp$' } | Sort-Object -Unique) # not core.rc: rc.exe's command is no compiler's
    if ($Base -and -not $fullRun) {
        $touched = @($touchedFiles | Where-Object { $_ -like 'native/*' } | ForEach-Object { [IO.Path]::GetFileName($_) })
        $headers = @($touched | Where-Object { $_ -match '\.h$' })
        $units = @($units | Where-Object {
            $touched -contains [IO.Path]::GetFileName($_) -or
            ($headers -and (Select-String -Path $_ -Pattern ($headers | ForEach-Object { [regex]::Escape($_) + '"' }) -Quiet))
        })
    }
    $analysed = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($u in $units) { [void]$analysed.Add((Rel $u).Substring(7)) }
    $clangTidy = Llvm 'clang-tidy.exe'
    if (-not $clangTidy) { throw "lint: clang-tidy $llvmMajor not found" }
    $tidyConfig = Join-Path $rules '.clang-tidy'
    $results = $units | ForEach-Object -ThrottleLimit ([Environment]::ProcessorCount) -Parallel {
        $unit = $_
        $errFile = [IO.Path]::GetTempFileName()
        try {
            $stdout = @(& $using:clangTidy -p $using:build "--config-file=$using:tidyConfig" --quiet $unit 2>$errFile | ForEach-Object { "$_" })
            $exit = $LASTEXITCODE
            $stderr = if (Test-Path $errFile) { [IO.File]::ReadAllLines($errFile) } else { @() }
        } finally {
            Remove-Item $errFile -ErrorAction SilentlyContinue
        }
        [pscustomobject]@{ Unit = $unit; Exit = $exit; Stdout = $stdout; Stderr = $stderr }
    }
    $seen = @{}
    foreach ($r in $results) {
        $unitRel = Rel $r.Unit
        $allLines = @($r.Stdout) + @($r.Stderr)
        if ($r.Exit -ne 0) {
            $errors.Add("tool-error: clang-tidy failed on $unitRel (exit $($r.Exit)): $(Tail $allLines)")
        }
        # Catch compile/fatal errors anywhere (including system headers) before the native/ path filter.
        foreach ($line in $allLines) {
            if ($line -match '^(?:(.+?):(\d+):\d+: )?(fatal error: .*)$') {
                $where = if ($Matches[1]) { "$(Rel $Matches[1]):$($Matches[2])" } else { $unitRel }
                Fail $where 'clang-diagnostic-error' "$($Matches[3]) (in $unitRel)"
            } elseif ($line -match '^(.+?):(\d+):\d+: error: (.*) \[(clang-diagnostic-[\w-]+)\]$') {
                Fail "$(Rel $Matches[1]):$($Matches[2])" $Matches[4] "$($Matches[3]) (in $unitRel)"
            }
        }
        $current = $null
        foreach ($line in $r.Stdout) {
            if ($line -match '^(.+?):(\d+):\d+: (?:warning|error): (.*) \[([\w.,-]+)\]$') {
                $file, $row, $text, $check = $Matches[1..4]
                $rel = Rel $file
                $current = $null
                if ($check -like 'clang-diagnostic-*' -and $line -match ': error: ') { continue }
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
                $current = $null
            }
        }
    }
}

# --- C#: the projects built with .editorconfig's style rules and the recommended analyzers as errors ----------
if ($DotNet) {
    foreach ($project in $projects) {
        $configs = if ($project -like '*Atrium.Settings.csproj') { @('', '-p:AtriumDevelopment=false') } else { @('') }
        foreach ($cfg in $configs) {
            $buildArgs = @('build', $project, '-c', 'Release', '-nologo') + $(if ($cfg) { @($cfg) } else { @() })
            $res = Invoke-Tool 'dotnet' $buildArgs -AllowNonZero
            $hits = 0
            $unparsedError = $false
            foreach ($line in $res.Lines) {
                if ($line -match '^\s*(.+?\.(?:cs|xaml))\((\d+),\d+\): (?:[\w]+\s+)?error (\w+): (.*?)( \[.*)?$') {
                    $hits++
                    Fail "$(Rel $Matches[1]):$($Matches[2])" $Matches[3] $Matches[4]
                } elseif ($line -match ': error | error [A-Z]+\d*:') {
                    $unparsedError = $true
                }
            }
            if ($res.ExitCode -ne 0 -and (-not $hits -or $unparsedError)) {
                $errors.Add("tool-error: dotnet build $(Rel $project) failed (exit $($res.ExitCode)): $(Tail $res.Lines)")
            }
        }
    }
}

# --- Ratchets ---------------------------------------------------------------------------------------------------
function Read-Baseline([hashtable]$map) {
    foreach ($k in @($map.Keys)) {
        $v = $map[$k]
        if (($v -is [int] -or $v -is [long]) -and $v -gt 0 -and $v -le [int]::MaxValue) { $map[$k] = [int]$v }
        else { Fail 'tools/lint-baseline.json' 'baseline' "$k must be a positive integer"; $map[$k] = 0 }
    }
    $map
}
function Match-BaselineMoves([System.Collections.IDictionary]$oldMap, [System.Collections.IDictionary]$newMap) {
    $shrank = @{}
    $isShrank = {
        param([string]$relNative)
        if ($shrank.Contains($relNative)) { return $shrank[$relNative] }
        $full = Join-Path $Root "native/$relNative"
        if (-not (Test-Path $full)) { return ($shrank[$relNative] = $true) }
        $cur = [IO.File]::ReadAllLines($full).Count
        $rev = if ($Base) { $mergeBase } else { 'HEAD' }
        $res = Invoke-Tool 'git' @('-C', $Root, 'show', "${rev}:native/$relNative") -AllowNonZero
        # A file added and split in the same change has no prior version here; it cannot count as shrunk.
        $old = if ($res.ExitCode -eq 0) { $res.Lines.Count }
            elseif ($oldMap.Contains("$relNative|file-lines")) { [int]$oldMap["$relNative|file-lines"] } else { 0 }
        $shrank[$relNative] = ($cur -lt $old)
        $shrank[$relNative]
    }
    $added = @($newMap.Keys | Where-Object { -not $oldMap.Contains($_) } | Sort-Object { -[int]$newMap[$_] }, { $_ })
    $removed = [System.Collections.Generic.List[string]]::new()
    foreach ($k in ($oldMap.Keys | Sort-Object)) { if (-not $newMap.Contains($k)) { $removed.Add($k) } }
    $matched = @{}
    foreach ($samePass in @($false, $true)) {
        foreach ($k in $added) {
            if ($matched.Contains($k)) { continue }
            $p = $k.Split('|', 3)
            if ($p.Count -lt 3) { continue }
            $best = -1
            for ($i = 0; $i -lt $removed.Count; $i++) {
                $rp = $removed[$i].Split('|', 3)
                if ($rp.Count -lt 3 -or $rp[1] -ne $p[1] -or [int]$newMap[$k] -gt [int]$oldMap[$removed[$i]]) { continue }
                $same = $rp[0] -eq $p[0]
                if ($same -ne $samePass) { continue }
                if (-not $same -and ($rp[2] -ne $p[2] -or $p[2] -in @('(lambda)', 'operator()') -or -not (& $isShrank $rp[0]))) { continue }
                if ($best -lt 0 -or [int]$oldMap[$removed[$i]] -lt [int]$oldMap[$removed[$best]]) { $best = $i }
            }
            if ($best -ge 0) { $matched[$k] = $removed[$best]; $removed.RemoveAt($best) }
        }
    }
    $matched
}
$baseline = Read-Baseline $(if (Test-Path $baselinePath) { Get-Content $baselinePath -Raw | ConvertFrom-Json -AsHashtable } else { @{} })
if ($Base) {
    $tree = Invoke-Tool 'git' @('-C', $Root, 'ls-tree', $mergeBase, '--', 'tools/lint-baseline.json') -AllowNonZero
    $gitOk = $tree.ExitCode -eq 0
    $baseBaseline = @{}
    if (-not $gitOk) {
        $errors.Add("tool-error: git ls-tree $mergeBase failed (exit $($tree.ExitCode)): $(Tail $tree.Lines)")
    } elseif ($tree.Lines | Where-Object { $_ }) {
        $res = Invoke-Tool 'git' @('-C', $Root, 'show', "${mergeBase}:tools/lint-baseline.json") -AllowNonZero
        if ($res.ExitCode -ne 0) {
            $gitOk = $false
            $errors.Add("tool-error: git show ${mergeBase}:tools/lint-baseline.json failed (exit $($res.ExitCode)): $(Tail $res.Lines)")
        } elseif ($res.Lines) {
            $baseBaseline = Read-Baseline (($res.Lines -join "`n") | ConvertFrom-Json -AsHashtable)
        }
    }
    if ($gitOk) {
        foreach ($k in $baseline.Keys) {
            if ($baseBaseline.Contains($k) -and [int]$baseline[$k] -gt [int]$baseBaseline[$k]) {
                Fail 'tools/lint-baseline.json' 'baseline' "$k rose ($($baseBaseline[$k]) -> $($baseline[$k]))"
            }
        }
        $baseMoves = Match-BaselineMoves $baseBaseline $baseline
        foreach ($k in $baseline.Keys) {
            if (-not $baseBaseline.Contains($k) -and -not $baseMoves.Contains($k)) {
                Fail 'tools/lint-baseline.json' 'baseline' "$k added without a matching removed key"
            }
        }
    }
}
$updateMoves = if ($UpdateBaseline) { Match-BaselineMoves $baseline $counts } else { @{} }
$advice = @{ 'file-lines' = "split it by what it does (at most $fileLines lines)"; 'complexity' = 'simplify it (cognitive complexity 40 at most)'
    'function-lines' = 'split it (120 lines at most)' }
foreach ($key in $counts.Keys) {
    if ($UpdateBaseline -and $updateMoves.Contains($key)) { continue }
    $limit = if ($baseline.Contains($key)) { [int]$baseline[$key] } else { 0 }
    if ([int]$counts[$key] -gt $limit) {
        $parts = $key.Split('|')
        $why = if ($ratcheted.Contains($parts[1])) { $ratcheted[$parts[1]][1] } else { $advice[$parts[1]] }
        Fail "native/$($parts[0])" "$($parts[1..($parts.Count - 1)] -join ' ')" "$($counts[$key]) > $limit; $why"
    }
}
if (-not $UpdateBaseline -and (-not $Paths -or (Test-Glob 'tools/lint-baseline.json' $Paths))) {
    foreach ($key in $baseline.Keys) {
        $tidyKey = $key -match '\|(complexity|function-lines)\|'
        if ($tidyKey -and -not $Tidy) { continue }
        if ($Base -and $tidyKey -and -not $fullRun) {
            $keyFile = $key.Split('|')[0]
            if ((Test-Path (Join-Path $Root "native/$keyFile")) -and -not $analysed.Contains($keyFile)) { continue }
        }
        if (-not $counts.Contains($key) -or [int]$counts[$key] -lt [int]$baseline[$key]) {
            $errors.Add("lint: $key is below its baseline; run ./tools/lint.ps1 -Tidy -UpdateBaseline")
        }
    }
}

# --- The change itself --------------------------------------------------------------------------------------------
if ($Base) {
    foreach ($line in $diffStatus) {
        $parts = $line -split "`t"
        $status = $parts[0]
        $checked = if ($status -like 'R*') { @($parts[1], $parts[2]) } elseif ($status -like 'C*') { @($parts[2]) } else { @($parts[1]) }
        foreach ($path in $checked) {
            if ($Paths -and -not (Test-Glob $path $Paths)) { Fail $path 'scope' "outside the allowed paths ($($Paths -join ', '))" }
        }
    }
    $numbers = $diffNumstat | ForEach-Object { ($_ -split "`t")[0..1] -replace '-', '0' }
    $total = ($numbers | ForEach-Object { [int]$_ } | Measure-Object -Sum).Sum
    if ($MaxLines -and $total -gt $MaxLines) { $errors.Add("diff: $total changed lines > budget $MaxLines") }
}

$problems = @($errors | Sort-Object -Unique)
# A scoped change answers for its own files (and its diff), never silencing tree-wide or tool errors.
if ($Paths) {
    $problems = @($problems | Where-Object {
        $file = if ($_ -match '^([^:]+?)(:\d+)?: ') { $Matches[1] } else { '' }
        -not $file -or $_ -match '^(lint|diff|tool-error): ' -or
            $_ -match ': (scope|version|baseline|clang-diagnostic-error|(?:CS|MC|WMC|RZ)\d+): ' -or (Test-Glob $file $Paths)
    })
}
$problems | ForEach-Object { Write-Output $_ }
if ($problems.Count) { Write-Output "lint: $($problems.Count) problem(s)"; exit 1 }
if ($UpdateBaseline) {
    $next = [ordered]@{}
    foreach ($key in $counts.Keys | Sort-Object) {
        if ($baseline.Contains($key)) { $next[$key] = [Math]::Min([int]$baseline[$key], [int]$counts[$key]) }
        elseif ($updateMoves.Contains($key)) { $next[$key] = [int]$counts[$key] }
    }
    [IO.File]::WriteAllText($baselinePath, ($next | ConvertTo-Json).Replace("`r`n", "`n") + "`n")
    "baseline: $($next.Count) entries"
    exit 0
}
Write-Output 'lint: clean'
