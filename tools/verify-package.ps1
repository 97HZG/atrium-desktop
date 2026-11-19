param([string]$ZipPath, [string]$Root = (Split-Path -Parent $PSScriptRoot), [switch]$SmokeStart)
$ErrorActionPreference = 'Stop'
if (!$ZipPath) {
    $candidates = @(Get-ChildItem -LiteralPath (Join-Path $Root 'dist') -Filter 'Atrium-*-win-x64.zip' -ErrorAction SilentlyContinue)
    if ($candidates.Count -ne 1) { throw "Expected 1 package zip in dist/, found $($candidates.Count)." }
    $ZipPath = $candidates[0].FullName
}
$ZipPath = [IO.Path]::GetFullPath($ZipPath)
if (!(Test-Path -LiteralPath $ZipPath -PathType Leaf)) { throw "Package zip not found: $ZipPath" }
$cmake = Get-Content -LiteralPath (Join-Path $Root 'native/CMakeLists.txt') -Raw
if ($cmake -notmatch 'VERSION\s+(\d+\.\d+\.\d+)') { throw 'Missing version in CMakeLists.txt.' }
$expectedVersion = $Matches[1]
$temp = Join-Path ([IO.Path]::GetFullPath([IO.Path]::GetTempPath())) ('Atrium-verify-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp | Out-Null
try {
    Expand-Archive -LiteralPath $ZipPath -DestinationPath $temp -Force
    $pkg = Join-Path $temp 'Atrium'; if (!(Test-Path -LiteralPath $pkg -PathType Container)) { $pkg = $temp }
    $required = @('Atrium.exe', 'Settings\Atrium.Settings.exe', 'Settings\Atrium.Settings.dll',
        'Settings\Update.ps1', 'Uninstall.ps1', 'LICENSE', 'locales\manifest.json')
    foreach ($rel in $required) {
        if (!(Test-Path -LiteralPath (Join-Path $pkg $rel) -PathType Leaf)) { throw "Missing package file: $rel" }
    }
    foreach ($ps in @('Uninstall.ps1', 'Settings\Update.ps1')) {
        $bom = [IO.File]::ReadAllBytes((Join-Path $pkg $ps))
        if ($bom.Length -lt 3 -or $bom[0] -ne 0xEF -or $bom[1] -ne 0xBB -or $bom[2] -ne 0xBF) { throw "$ps is missing its UTF-8 BOM." }
    }
    $testLocales = @(Get-ChildItem -LiteralPath (Join-Path $pkg 'locales') -Filter 'x-*.json' -ErrorAction SilentlyContinue)
    if ($testLocales.Count -gt 0) { throw "Test locale files found in package: $($testLocales.Name -join ', ')" }
    $manifest = Get-Content -LiteralPath (Join-Path $pkg 'locales\manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach ($lang in @($manifest.languages)) { if ($lang.tag -like 'x-*') { throw "Test locale tag found in manifest: $($lang.tag)" } }
    foreach ($bin in @('Atrium.exe', 'Settings\Atrium.Settings.dll')) {
        $info = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $pkg $bin))
        if ("$($info.FileMajorPart).$($info.FileMinorPart).$($info.FileBuildPart)" -ne $expectedVersion) { throw "$bin version != $expectedVersion." }
    }
    $exeBytes = (Get-Item -LiteralPath (Join-Path $pkg 'Atrium.exe')).Length
    if ($exeBytes -gt 4MB) { throw "Atrium.exe exceeds 4 MB limit: $exeBytes bytes." }
    $zipBytes = (Get-Item -LiteralPath $ZipPath).Length
    if ($zipBytes -gt 120MB) { throw "Package zip exceeds 120 MB limit: $zipBytes bytes." }
    if ($SmokeStart) {
        function Send-PipeRequest([System.IO.Pipes.NamedPipeClientStream]$pipe, [string]$json) {
            $req = [Text.Encoding]::UTF8.GetBytes($json)
            $pipe.Write($req, 0, $req.Length)
            $pipe.Flush()
            $ms = New-Object System.IO.MemoryStream
            try {
                $buf = New-Object byte[] 4096
                do {
                    $n = $pipe.Read($buf, 0, $buf.Length)
                    if ($n -eq 0) { throw 'Pipe closed unexpectedly while reading response.' }
                    $ms.Write($buf, 0, $n)
                } while (!$pipe.IsMessageComplete)
                [Text.Encoding]::UTF8.GetString($ms.ToArray()) | ConvertFrom-Json
            } finally {
                $ms.Dispose()
            }
        }
        $running = @(Get-Process -Name Atrium -ErrorAction SilentlyContinue)
        if ($running.Count -gt 0) {
            throw "Atrium is already running (PID: $($running.Id -join ', ')); stop it before -SmokeStart."
        }
        $appDataDir = Join-Path $env:LOCALAPPDATA 'Atrium'
        $settingsPath = Join-Path $appDataDir 'settings.json'
        $hadSettings = Test-Path -LiteralPath $settingsPath -PathType Leaf
        $savedSettingsBytes = if ($hadSettings) { [IO.File]::ReadAllBytes($settingsPath) } else { $null }
        $logPath = Join-Path $appDataDir 'logs\core.log'
        $initialLogLen = if (Test-Path -LiteralPath $logPath -PathType Leaf) { (Get-Item -LiteralPath $logPath).Length } else { 0L }
        $core = Join-Path $pkg 'Atrium.exe'
        $eventName = 'Local\Atrium.Update.Core.' + [Guid]::NewGuid().ToString('N')
        $ready = New-Object System.Threading.EventWaitHandle($false, [Threading.EventResetMode]::ManualReset, $eventName)
        $proc = $null
        try {
            $proc = Start-Process -FilePath $core -ArgumentList "--development --ready-event $eventName" `
                -WorkingDirectory $temp -WindowStyle Hidden -PassThru
            if (!$ready.WaitOne(15000) -or $proc.HasExited) { throw 'Atrium.exe failed smoke startup.' }
            $pipeName = "Atrium.Core.$([Diagnostics.Process]::GetCurrentProcess().SessionId)"
            $pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', $pipeName, [System.IO.Pipes.PipeDirection]::InOut)
            try {
                $pipe.Connect(5000)
                $pipe.ReadMode = [System.IO.Pipes.PipeTransmissionMode]::Message
                $state = Send-PipeRequest $pipe '{"op":"get"}'
                if ($state.op -ne 'state' -or [int]$state.protocol -ne 1) {
                    throw "Unexpected get reply: $($state | ConvertTo-Json -Compress)"
                }
                $savedHeight = [double]$state.saved.height
                $previewHeight = if ($savedHeight -eq 72) { 64 } else { 72 }
                $previewReq = "{`"op`":`"preview`",`"settings`":{`"height`":$previewHeight}}"
                $prevReply = Send-PipeRequest $pipe $previewReq
                if ($prevReply.op -ne 'ok') { throw "Preview failed: $($prevReply | ConvertTo-Json -Compress)" }
                $during = Send-PipeRequest $pipe '{"op":"get"}'
                if ([double]$during.current.height -ne $previewHeight -or [double]$during.saved.height -ne $savedHeight) {
                    throw "Preview state mismatch: current=$($during.current.height), saved=$($during.saved.height)"
                }
            } finally {
                $pipe.Dispose()
            }
            Start-Sleep -Milliseconds 300
            $pipe2 = New-Object System.IO.Pipes.NamedPipeClientStream('.', $pipeName, [System.IO.Pipes.PipeDirection]::InOut)
            try {
                $pipe2.Connect(5000)
                $pipe2.ReadMode = [System.IO.Pipes.PipeTransmissionMode]::Message
                $after = Send-PipeRequest $pipe2 '{"op":"get"}'
                if ([double]$after.current.height -ne $savedHeight) {
                    throw "Preview lease did not roll back: current=$($after.current.height), expected=$savedHeight"
                }
            } finally {
                $pipe2.Dispose()
            }
            $exit = Start-Process -FilePath $core -ArgumentList '--exit' -WorkingDirectory $temp -WindowStyle Hidden -PassThru
            try {
                $exitDone = $exit.WaitForExit(15000)
                $exitCode = if ($exitDone) { $exit.ExitCode } else { 'timeout' }
                if (!$exitDone -or $exitCode -ne 0 -or !$proc.WaitForExit(15000)) {
                    $tail = if (Test-Path -LiteralPath $logPath -PathType Leaf) {
                        (Get-Content -LiteralPath $logPath -Tail 30 -Encoding UTF8) -join "`n"
                    } else { '(core.log missing)' }
                    throw "Atrium.exe --exit failed (exitCode=$exitCode, exited=$($proc.HasExited)).`ncore.log tail:`n$tail"
                }
            } finally {
                $exit.Dispose()
            }
            if (!(Test-Path -LiteralPath $logPath -PathType Leaf)) { throw "Missing core.log: $logPath" }
            $fs = [IO.File]::Open($logPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
            try {
                $offset = if ($fs.Length -lt $initialLogLen) { 0L } else { $initialLogLen }
                [void]$fs.Seek($offset, [IO.SeekOrigin]::Begin)
                $sr = New-Object IO.StreamReader($fs, [Text.Encoding]::UTF8, $true)
                try { $logDelta = $sr.ReadToEnd() } finally { $sr.Dispose() }
            } finally {
                $fs.Dispose()
            }
            if ($logDelta -notmatch 'core: starting' -or $logDelta -notmatch 'core: exited') {
                throw "core.log missing expected startup/exit lines.`nDelta:`n$logDelta"
            }
            if ($logDelta -match 'render thread stuck') {
                throw "core.log reported render thread stuck.`nDelta:`n$logDelta"
            }
        } finally {
            if ($proc -and !$proc.HasExited) {
                Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
                [void]$proc.WaitForExit(3000)
            }
            if ($proc) { $proc.Dispose() }
            $ready.Dispose()
            if ($hadSettings) {
                if (!(Test-Path -LiteralPath $appDataDir -PathType Container)) {
                    New-Item -ItemType Directory -Path $appDataDir -Force | Out-Null
                }
                [IO.File]::WriteAllBytes($settingsPath, $savedSettingsBytes)
            } elseif (Test-Path -LiteralPath $settingsPath -PathType Leaf) {
                Remove-Item -LiteralPath $settingsPath -Force -ErrorAction SilentlyContinue
            }
        }
    }
    Write-Host "Package verified: $ZipPath ($expectedVersion)"
} finally {
    if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue }
}
