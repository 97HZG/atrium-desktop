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
    if ($SmokeStart) {
        $core = Join-Path $pkg 'Atrium.exe'; $eventName = 'Local\Atrium.Verify.' + [Guid]::NewGuid().ToString('N')
        $ready = New-Object System.Threading.EventWaitHandle($false, [Threading.EventResetMode]::ManualReset, $eventName); $proc = $null
        try {
            $proc = Start-Process -FilePath $core -ArgumentList "--ready-event $eventName" -WorkingDirectory $temp -WindowStyle Hidden -PassThru
            if (!$ready.WaitOne(10000) -or $proc.HasExited) { throw 'Atrium.exe failed smoke startup.' }
            $exit = Start-Process -FilePath $core -ArgumentList '--exit' -WorkingDirectory $temp -WindowStyle Hidden -PassThru
            try { if (!$exit.WaitForExit(15000) -or $exit.ExitCode -ne 0) { throw 'Atrium.exe --exit failed during smoke test.' } } finally { $exit.Dispose() }
        } finally {
            if ($proc -and !$proc.HasExited) {
                Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
                [void]$proc.WaitForExit(3000)
            }
            if ($proc) { $proc.Dispose() }; $ready.Dispose()
        }
    }
    Write-Host "Package verified: $ZipPath ($expectedVersion)"
} finally {
    if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue }
}
