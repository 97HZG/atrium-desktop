# Clang-tidy pipeline helper dot-sourced by tools/lint.ps1; never execute standalone.

function Get-VsPath {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) { & $vswhere -latest -products * -property installationPath 2>$null } else { $null }
}

function Find-Ninja {
    $cmd = Get-Command ninja.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($cmd) { return $cmd.Source }
    $vs = Get-VsPath
    if ($vs) {
        $cand = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
        if (Test-Path $cand) { return $cand }
    }
    $null
}

function Import-MsvcEnv {
    $cl = Get-Command cl.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($cl -and $cl.Source -match '[\\/]x64[\\/]cl\.exe$') { return }
    $vs = Get-VsPath
    $vcvars = if ($vs) { Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat' } else { $null }
    if (-not $vcvars -or -not (Test-Path $vcvars)) { throw 'lint: cl.exe not on PATH and Visual Studio vcvars64.bat not found' }
    foreach ($line in (Invoke-Tool 'cmd' @('/c', "`"$vcvars`" >nul && set")).Lines) {
        if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2]) }
    }
}

function Resolve-BuildPath([string]$BaseDir, [string]$RelOrAbs) {
    [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($RelOrAbs)) { $RelOrAbs } else { Join-Path $BaseDir $RelOrAbs }))
}

function Get-TouchedShaderHeaders([string]$Build, [System.Collections.Generic.HashSet[string]]$TouchedSet) {
    $genHeaders = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $hasHlsl = [bool]($TouchedSet | Where-Object { $_ -match '^native/core/render/shaders/.*\.hlsl$' } | Select-Object -First 1)
    $buildNinja = Join-Path $Build 'build.ninja'
    if (-not $hasHlsl -or -not (Test-Path -LiteralPath $buildNinja -PathType Leaf)) { return ,$genHeaders }
    foreach ($line in [IO.File]::ReadLines($buildNinja)) {
        if ($line -match '^build\s+([^\s:|]+[\\/]shaders[\\/][^:|]+\.h)\b(?:[^:$]|\$.)*:\s+\S+\s+(.+)$') {
            $genRel = $Matches[1].Replace('$:', ':').Replace('$ ', ' ').Replace('$$', '$')
            foreach ($tok in ($Matches[2] -split '(?<!\$)\s+')) {
                if (-not $tok -or $tok -eq '|' -or $tok -eq '||') { continue }
                $dep = $tok.Replace('$:', ':').Replace('$ ', ' ').Replace('$$', '$')
                if ($TouchedSet.Contains((Rel (Resolve-BuildPath $Build $dep)))) { [void]$genHeaders.Add((Resolve-BuildPath $Build $genRel)); break }
            }
        }
    }
    return ,$genHeaders
}

function Get-StaticHeaders([string]$StartFile, [string]$Root, [hashtable]$DirectCache) {
    $coreDir = Join-Path $Root 'native/core'
    $visited = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $queue = [System.Collections.Generic.Queue[string]]::new()
    $queue.Enqueue($StartFile)
    while ($queue.Count -gt 0) {
        $cur = $queue.Dequeue()
        if (-not $DirectCache.ContainsKey($cur)) {
            $incs = [System.Collections.Generic.List[string]]::new()
            $curDir = Split-Path $cur
            if (Test-Path -LiteralPath $cur -PathType Leaf) {
                foreach ($line in [IO.File]::ReadLines($cur)) {
                    if ($line -match '^\s*#\s*include\s*[<"]([^>"]+)[>"]') {
                        $cand = [IO.Path]::GetFullPath((Join-Path $curDir $Matches[1]))
                        if (-not (Test-Path -LiteralPath $cand -PathType Leaf)) { $cand = [IO.Path]::GetFullPath((Join-Path $coreDir $Matches[1])) }
                        if ((Test-Path -LiteralPath $cand -PathType Leaf) -and (Rel $cand) -match '^native/(?:core|tests)/.*\.h$') { $incs.Add($cand) }
                    }
                }
            }
            $DirectCache[$cur] = $incs
        }
        foreach ($next in $DirectCache[$cur]) { if ($visited.Add($next)) { $queue.Enqueue($next) } }
    }
    return ,$visited
}

function Test-HasDevBranch([string]$Unit, $Headers, [hashtable]$DevFileCache) {
    $hasDev = {
        param([string]$path)
        if (-not $DevFileCache.ContainsKey($path)) {
            $DevFileCache[$path] = (Test-Path -LiteralPath $path -PathType Leaf) -and
                ([IO.File]::ReadAllText($path) -match '(?m)^\s*#\s*(?:el)?if(?:n?def)?\b.*\bATRIUM_DEVELOPMENT\b')
        }
        $DevFileCache[$path]
    }
    if (& $hasDev $Unit) { return $true }
    foreach ($h in $Headers) { if (& $hasDev $h) { return $true } }
    $false
}

function Invoke-TidyCheck(
    [string]$Root, [string]$Rules, [string]$BuildDir, [string]$Base, [bool]$FullRun, [string[]]$TouchedFiles,
    [System.Collections.Generic.HashSet[string]]$Analysed, [System.Collections.Generic.List[string]]$Errors,
    [System.Collections.IDictionary]$Counts, [int]$LlvmMajor
) {
    Import-MsvcEnv
    $ninja = Find-Ninja
    if ($BuildDir) {
        $build = Resolve-BuildPath $Root $BuildDir
        if (-not (Test-Path -LiteralPath (Join-Path $build 'compile_commands.json') -PathType Leaf)) {
            throw "lint: -BuildDir '$BuildDir' has no compile_commands.json"
        }
    } else {
        $build = Join-Path $Root 'native/build/tidy'
        $cmakeArgs = @('-S', (Join-Path $Root 'native'), '-B', $build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
            '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', '-DATRIUM_DEVELOPMENT=OFF', '-DATRIUM_BUILD_TESTS=ON')
        if ($ninja) { $cmakeArgs += "-DCMAKE_MAKE_PROGRAM=$ninja" }
        Invoke-Tool 'cmake' $cmakeArgs | Out-Null
        Invoke-Tool 'cmake' @('--build', $build) | Out-Null # the generated shader headers
    }

    $ccEntries = @((Get-Content -LiteralPath (Join-Path $build 'compile_commands.json') -Raw | ConvertFrom-Json))
    $unitSet = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $objToUnit = @{}; $unitDevDefined = @{}
    foreach ($entry in $ccEntries) {
        $f = [string]$entry.file
        if ($f -notmatch '[\\/]native[\\/](core|tests)[\\/].*\.cpp$') { continue }
        $u = [IO.Path]::GetFullPath($f)
        [void]$unitSet.Add($u)
        if ($entry.output) { $objToUnit[(([string]$entry.output).Replace('\', '/') -replace '^\./', '')] = $u }
        $cmd = if ($entry.command) { [string]$entry.command } elseif ($entry.arguments) { $entry.arguments -join ' ' } else { '' }
        $dev = $false
        foreach ($m in [regex]::Matches($cmd, '(?:^|[\s"])[-/](D|U)ATRIUM_DEVELOPMENT(?:=([^\s"]+))?(?=[\s"]|$)')) {
            $dev = ($m.Groups[1].Value -ne 'U') -and (-not $m.Groups[2].Success -or $m.Groups[2].Value -ne '0')
        }
        $unitDevDefined[$u] = $dev
    }
    $units = @($unitSet | Sort-Object)

    $touchedSet = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($tf in $TouchedFiles) { if ($tf) { [void]$touchedSet.Add(($tf.Replace('\', '/') -replace '^\./', '')) } }
    $unitHeaders = @{}
    $selectedSet = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $seenUnits = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    if ($ninja -and (Test-Path -LiteralPath (Join-Path $build '.ninja_deps') -PathType Leaf)) {
        $touchedGen = if ($Base -and -not $FullRun) { Get-TouchedShaderHeaders $build $touchedSet } else { $null }
        $curUnit = $null
        foreach ($line in (Invoke-Tool $ninja @('-C', $build, '-t', 'deps')).Lines) {
            if ($line -match '^(\S.*?): #deps ') {
                $curUnit = $objToUnit[(($Matches[1].Replace('\', '/') -replace '^\./', '').Trim())]
                if ($curUnit -and $line -match ': #deps [1-9]\d*') { [void]$seenUnits.Add($curUnit) } else { $curUnit = $null }
                continue
            }
            if (-not $line.StartsWith(' ') -or -not $curUnit) { if (-not $line.StartsWith(' ')) { $curUnit = $null }; continue }
            $dep = $line.Trim(); if (-not $dep) { continue }
            $depAbs = Resolve-BuildPath $build $dep; $depRel = Rel $depAbs
            if ($depRel -match '^native/(?:core|tests)/.*\.h$') {
                if (-not $unitHeaders.ContainsKey($curUnit)) {
                    $unitHeaders[$curUnit] = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
                }
                [void]$unitHeaders[$curUnit].Add($depAbs)
            }
            if ($Base -and -not $FullRun -and ($touchedSet.Contains($depRel) -or ($touchedGen -and $touchedGen.Contains($depAbs)))) {
                [void]$selectedSet.Add($curUnit)
            }
        }
        if ($Base -and -not $FullRun) {
            $units = @($units | Where-Object { $touchedSet.Contains((Rel $_)) -or $selectedSet.Contains($_) -or -not $seenUnits.Contains($_) })
        }
    } else {
        $directCache = @{}
        $hasHlsl = [bool]($touchedSet | Where-Object { $_ -match '^native/core/render/shaders/.*\.hlsl$' } | Select-Object -First 1)
        foreach ($u in $units) { $unitHeaders[$u] = Get-StaticHeaders $u $Root $directCache }
        if ($Base -and -not $FullRun) {
            $units = @($units | Where-Object {
                $uRel = Rel $_
                $touchedSet.Contains($uRel) -or ($hasHlsl -and $uRel -eq 'native/core/render/glass_renderer.cpp') -or
                    [bool]($unitHeaders[$_] | Where-Object { $touchedSet.Contains((Rel $_)) } | Select-Object -First 1)
            })
        }
    }

    foreach ($u in $units) { $r = Rel $u; if ($r.StartsWith('native/')) { [void]$Analysed.Add($r.Substring(7)) } }
    $clangTidy = Llvm 'clang-tidy.exe'
    if (-not $clangTidy) { throw "lint: clang-tidy $LlvmMajor not found" }
    $tidyConfig = Join-Path $Rules '.clang-tidy'; $devFileCache = @{}
    $jobs = @(foreach ($u in $units) {
        $hdrs = if ($unitHeaders.ContainsKey($u)) { $unitHeaders[$u] } else { @() }
        $hasBranch = Test-HasDevBranch $u $hdrs $devFileCache; $devDef = [bool]$unitDevDefined[$u]
        [pscustomobject]@{ Unit = $u; ExtraArg = ''; RecordRatchets = (-not $hasBranch) -or (-not $devDef) }
        if ($hasBranch) {
            [pscustomobject]@{ Unit = $u; ExtraArg = $(if ($devDef) { '-UATRIUM_DEVELOPMENT' } else { '-DATRIUM_DEVELOPMENT' }); RecordRatchets = $devDef }
        }
    })
    $results = if ($jobs.Count) {
        @($jobs | ForEach-Object -ThrottleLimit ([Environment]::ProcessorCount) -Parallel {
            $job = $_; $errFile = [IO.Path]::GetTempFileName()
            try {
                $argsList = @('-p', $using:build, "--config-file=$using:tidyConfig", '--quiet')
                if ($job.ExtraArg) { $argsList += "--extra-arg=$($job.ExtraArg)" }
                $stdout = @(& $using:clangTidy @argsList $job.Unit 2>$errFile | ForEach-Object { "$_" })
                $exit = $LASTEXITCODE
                $stderr = if (Test-Path $errFile) { [IO.File]::ReadAllLines($errFile) } else { @() }
            } finally { Remove-Item $errFile -ErrorAction SilentlyContinue }
            [pscustomobject]@{ Unit = $job.Unit; ExtraArg = $job.ExtraArg; RecordRatchets = $job.RecordRatchets
                Exit = $exit; Stdout = $stdout; Stderr = $stderr }
        })
    } else { @() }

    $seen = @{}
    foreach ($r in $results) {
        $unitRel = Rel $r.Unit; $tag = if ($r.ExtraArg) { "$unitRel ($($r.ExtraArg))" } else { $unitRel }
        $allLines = @($r.Stdout) + @($r.Stderr)
        if ($r.Exit -ne 0) { $Errors.Add("tool-error: clang-tidy failed on $tag (exit $($r.Exit)): $(Tail $allLines)") }
        foreach ($line in $allLines) {
            if ($line -match '^(?:(.+?):(\d+):\d+: )?(fatal error: .*)$') {
                Fail $(if ($Matches[1]) { "$(Rel $Matches[1]):$($Matches[2])" } else { $unitRel }) 'clang-diagnostic-error' "$($Matches[3]) (in $tag)"
            } elseif ($line -match '^(.+?):(\d+):\d+: error: (.*) \[(clang-diagnostic-[\w-]+)\]$') {
                Fail "$(Rel $Matches[1]):$($Matches[2])" $Matches[4] "$($Matches[3]) (in $tag)"
            }
        }
        $current = $null
        foreach ($line in $r.Stdout) {
            if ($line -match '^(.+?):(\d+):\d+: (?:warning|error): (.*) \[([\w.,-]+)\]$') {
                $file, $row, $text, $check = $Matches[1..4]; $rel = Rel $file; $current = $null
                if ($check -like 'clang-diagnostic-*' -and $line -match ': error: ') { continue }
                if (-not $rel.StartsWith('native/core/') -and -not $rel.StartsWith('native/tests/')) { continue }
                $where = "${rel}:$row"
                $isRatchet = $check -in @('readability-function-cognitive-complexity', 'readability-function-size')
                if (($isRatchet -and -not $r.RecordRatchets) -or $seen.ContainsKey("$where|$check")) { continue }
                $seen["$where|$check"] = $true
                if ($check -eq 'readability-function-cognitive-complexity' -and $text -match "function '(.+)' has cognitive complexity of (\d+)") {
                    $Counts["$($rel.Substring(7))|complexity|$($Matches[1])"] = [int]$Matches[2]
                } elseif ($check -eq 'readability-function-cognitive-complexity' -and $text -match 'lambda has cognitive complexity of (\d+)') {
                    $key = "$($rel.Substring(7))|complexity|(lambda)"
                    $Counts[$key] = [Math]::Max([int]$Counts[$key], [int]$Matches[1])
                } elseif ($check -eq 'readability-function-size' -and $text -match "function '(.+)' exceeds") {
                    $current = "$($rel.Substring(7))|function-lines|$($Matches[1])"; $Counts[$current] = 0
                } else { Fail $where $check $text }
            } elseif ($current -and $line -match 'note: (\d+) lines including') {
                $Counts[$current] = [int]$Matches[1]; $current = $null
            }
        }
    }
}
