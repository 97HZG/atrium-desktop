# Launched only after the user chooses to restart. Runs outside the install folder.
param(
    [Parameter(Mandatory)][string]$InstallDirectory,
    [Parameter(Mandatory)][string]$PackageDirectory,
    [Parameter(Mandatory)][string]$StatePath,
    [Parameter(Mandatory)][int]$WaitForProcessId,
    [ValidateSet('0', '1')][string]$StartCore = '1',
    [ValidateRange(1, 60)][int]$StartupTimeoutSeconds = 20
)
$ErrorActionPreference = 'Stop'
$install = [IO.Path]::GetFullPath($InstallDirectory).TrimEnd('\')
$package = [IO.Path]::GetFullPath($PackageDirectory).TrimEnd('\')
$StatePath = [IO.Path]::GetFullPath($StatePath)
$cache = Split-Path -Parent $StatePath
$parent = Split-Path -Parent $install
$name = Split-Path -Leaf $install
$suffix = [Guid]::NewGuid().ToString('N')
$incoming = Join-Path $parent "$name.update-$suffix"
$backup = Join-Path $parent "$name.previous-$suffix"
$result = Join-Path $cache 'result.json'
$coreStopped = $false
$backupStarted = $false
$incomingStarted = $false
$confirmed = $false
$restored = $false
$started = New-Object 'System.Collections.Generic.List[System.Diagnostics.Process]'
# Never the install folder: what starts there would hold it, and the next update could not replace it.
$workingDirectory = [Environment]::GetFolderPath('UserProfile')

function Assert-TemporaryPath([string]$Path) {
    $absolute = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    if ((Split-Path -Parent $absolute) -ne $parent -or
        ((Split-Path -Leaf $absolute) -ne "$name.update-$suffix" -and
         (Split-Path -Leaf $absolute) -ne "$name.previous-$suffix")) { throw '更新临时目录无效。' }
}

# Moves what is in $From into $To (merging folders). A folder some process is in (its current
# directory, e.g. an app started from an older Atrium) cannot be moved itself: its contents go, it stays.
function Move-Tree([string]$From, [string]$To, [switch]$Keep, [switch]$BestEffort, [System.Collections.Generic.List[string]]$Errors = $null) {
    if (!(Test-Path -LiteralPath $To)) { New-Item -ItemType Directory -Path $To | Out-Null }
    foreach ($item in @(Get-ChildItem -LiteralPath $From -Force)) {
        $target = Join-Path $To $item.Name
        if ($item.PSIsContainer -and (Test-Path -LiteralPath $target -PathType Container)) {
            Move-Tree $item.FullName $target -BestEffort:$BestEffort -Errors $Errors; continue
        }
        if ($item.PSIsContainer) {
            try { Move-Item -LiteralPath $item.FullName -Destination $target -Force }
            catch { Move-Tree $item.FullName $target -BestEffort:$BestEffort -Errors $Errors }
            continue
        }
        # A file just written may be open for a moment (a virus scanner reading it).
        $maxTries = if ($BestEffort) { 3 } else { 6 }
        for ($try = 1; ; $try++) {
            try { Move-Item -LiteralPath $item.FullName -Destination $target -Force; break }
            catch {
                if ($try -lt $maxTries) { Start-Sleep -Milliseconds 300; continue }
                $message = "无法替换 $($item.FullName)（$($_.Exception.Message.TrimEnd('。', '.'))），请关闭正在使用它的程序后重试。"
                if ($BestEffort) { if ($Errors) { $Errors.Add($message) }; break }
                throw $message
            }
        }
    }
    if (!$Keep) {
        try {
            [IO.Directory]::Delete($From, $false)
        } catch [UnauthorizedAccessException] {
            try {
                [IO.File]::SetAttributes($From, ([IO.File]::GetAttributes($From) -band (-bnot [IO.FileAttributes]::ReadOnly)))
                [IO.Directory]::Delete($From, $false)
            } catch { }
        } catch { }
    }
}

# What settings wrote to its crash log since $Since (its exception's first line), for the message.
function Get-CrashCause([datetime]$Since) {
    $log = Join-Path $env:LOCALAPPDATA 'Atrium\logs\settings.log'
    try {
        if (!(Test-Path -LiteralPath $log) -or (Get-Item -LiteralPath $log).LastWriteTime -lt $Since) { return '' }
        $last = Get-Content -LiteralPath $log -Tail 60 -Encoding UTF8 |
            Where-Object { $_ -match '^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} ' } | Select-Object -Last 1
        if (!$last) { return '' }
        $cause = $last.Substring(20).Trim()
        if ($cause.Length -gt 200) { $cause = $cause.Substring(0, 200) + '...' }
        return "：$cause"
    } catch { return '' }
}

# Smart App Control or an App Control (WDAC) policy refused a file: ERROR_SYSTEM_INTEGRITY_* 4550-4559 and
# 4580-4582. Windows PowerShell's Start-Process keeps only the system message, not the code, so compare that.
function Test-AppControlBlock([string]$Message) {
    foreach ($code in @(4550..4559) + @(4580..4582)) {
        $text = ([ComponentModel.Win32Exception]::new($code)).Message.Trim().TrimEnd('。', '.')
        if ($text -and $Message.Contains($text)) { return $true }
    }
    return $false
}

function Start-Verified([string]$Executable, [string]$Kind, [string]$Arguments) {
    $eventName = "Local\Atrium.Update.$Kind.$suffix"
    $ready = New-Object System.Threading.EventWaitHandle($false, [Threading.EventResetMode]::ManualReset, $eventName)
    try {
        $startedAt = (Get-Date).AddSeconds(-1)
        try {
            $process = Start-Process -FilePath $Executable -ArgumentList "$Arguments --ready-event $eventName" -WorkingDirectory $workingDirectory `
                -WindowStyle Hidden -PassThru
        } catch {
            if (!(Test-AppControlBlock $_.Exception.Message)) { throw }
            # The release is not code-signed; each version's new files have no reputation yet.
            throw ("Windows 应用程序控制阻止了新版 $Kind 运行，已保留旧版本。新版文件没有代码签名，可能被智能应用控制或单位的应用控制策略拦截：" +
                   "可在 Windows 安全中心 › 应用和浏览器控制 › 智能应用控制中查看；单位电脑请联系管理员放行 Atrium 所在目录。（$($_.Exception.Message.Trim())）")
        }
        $started.Add($process)
        $until = [DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds)
        do {
            if ($process.HasExited) {
                $cause = if ($Kind -eq 'Settings') { Get-CrashCause $startedAt } else { '' }
                throw ("$Kind 启动失败（退出码 0x{0:X8}）{1}。" -f $process.ExitCode, $cause)
            }
            if ($ready.WaitOne(100)) {
                if ($process.HasExited) { throw "$Kind 在就绪后退出。" }
                return
            }
        } while ([DateTime]::UtcNow -lt $until)
        throw "$Kind 未确认启动就绪，已保留旧版本。"
    } finally { $ready.Dispose() }
}

function Get-InstallProcesses {
    $prefix = $install + '\'
    $byId = @{}
    foreach ($process in @(Get-Process -ErrorAction SilentlyContinue)) {
        try {
            if ($process.Id -ne $PID -and $process.Path -and
                $process.Path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
                $byId[$process.Id] = [pscustomobject]@{ Process = $process; Path = $process.Path }
            }
        } catch { }
    }
    try {
        foreach ($cim in @(Get-CimInstance -ClassName Win32_Process -ErrorAction Stop)) {
            $procId = [int]$cim.ProcessId
            if ($procId -ne $PID -and $cim.ExecutablePath -and
                $cim.ExecutablePath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -and !$byId.ContainsKey($procId)) {
                $process = Get-Process -Id $procId -ErrorAction SilentlyContinue
                if ($process) { $byId[$procId] = [pscustomobject]@{ Process = $process; Path = $cim.ExecutablePath } }
            }
        }
    } catch { }
    return @($byId.Values)
}

function Stop-InstallProcesses([int]$WaitMilliseconds, [System.Diagnostics.Process[]]$Tracked = @()) {
    for ($pass = 1; $pass -le 2; $pass++) {
        $byId = @{}
        foreach ($process in @($Tracked) + @(Get-InstallProcesses | ForEach-Object { $_.Process })) {
            try {
                if ($process -and $process.Id -ne $PID -and !$process.HasExited -and !$byId.ContainsKey($process.Id)) { $byId[$process.Id] = $process }
            } catch { }
        }
        $processes = @($byId.Values)
        if ($processes.Count -eq 0) { return }
        if ($pass -eq 1 -and $WaitMilliseconds -gt 0) {
            $until = [DateTime]::UtcNow.AddMilliseconds($WaitMilliseconds)
            while ([DateTime]::UtcNow -lt $until) {
                $alive = $false; foreach ($process in $processes) { try { if (!$process.HasExited) { $alive = $true; break } } catch { } }
                if (!$alive) { break }; Start-Sleep -Milliseconds 100
            }
        }
        foreach ($process in $processes) {
            try { if (!$process.HasExited) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue; [void]$process.WaitForExit(3000) } } catch { }
        }
    }
}

function Write-Result([string]$ErrorText) {
    $temporary = $result + '.tmp'
    @{ error = $ErrorText } | ConvertTo-Json | Set-Content -LiteralPath $temporary -Encoding UTF8
    Move-Item -LiteralPath $temporary -Destination $result -Force
}

try {
    if (!$parent -or !$name -or $install -eq [IO.Path]::GetPathRoot($install).TrimEnd('\') -or
        !$package.StartsWith($cache + '\', [StringComparison]::OrdinalIgnoreCase)) { throw '更新目录无效。' }
    Assert-TemporaryPath $incoming
    Assert-TemporaryPath $backup
    foreach ($root in @($install, $package)) {
        if (!(Test-Path -LiteralPath (Join-Path $root 'Atrium.exe')) -or
            !(Test-Path -LiteralPath (Join-Path $root 'Settings\Atrium.Settings.exe'))) { throw '找不到完整的 Atrium 程序文件。' }
    }
    # Copy before stopping the app; disk/access failures leave the old app running.
    New-Item -ItemType Directory -Path $incoming | Out-Null
    Get-ChildItem -LiteralPath $package -Force | Copy-Item -Destination $incoming -Recurse -Force
    # A downloaded release must retain the development package's explicit identity.
    if (Test-Path -LiteralPath (Join-Path $install 'development.txt')) {
        Copy-Item -LiteralPath (Join-Path $install 'development.txt') -Destination $incoming -Force
    }
    $settingsProcess = Get-Process -Id $WaitForProcessId -ErrorAction SilentlyContinue
    if ($settingsProcess -and !$settingsProcess.WaitForExit(30000)) { throw '设置中心尚未退出，更新未安装。' }
    $coreExe = Join-Path $install 'Atrium.exe'
    $coreRunning = @(Get-InstallProcesses | Where-Object { $_.Path -and [string]::Equals($_.Path, $coreExe, [StringComparison]::OrdinalIgnoreCase) })
    if ($StartCore -eq '1' -or $coreRunning.Count -gt 0) {
        $quit = Start-Process -FilePath $coreExe -ArgumentList '--exit' -WorkingDirectory $workingDirectory -WindowStyle Hidden -PassThru
        try {
            if (!$quit.WaitForExit(15000) -or $quit.ExitCode -ne 0) { throw 'Atrium 尚未正常退出，更新未安装。' }
        } finally { $quit.Dispose() }
        $coreStopped = $true
    }
    Stop-InstallProcesses -WaitMilliseconds 5000
    Assert-TemporaryPath $backup
    Assert-TemporaryPath $incoming
    # The folder itself stays (an app may be in it); what is in it is replaced.
    $backupStarted = $true
    Move-Tree $install $backup -Keep
    $incomingStarted = $true
    Move-Tree $incoming $install
    if ($StartCore -eq '1') { Start-Verified (Join-Path $install 'Atrium.exe') 'Core' '' }
    $settingsArguments = if ($StartCore -eq '1') { '--page about --require-core-ready' } else { '--page about' }
    Start-Verified (Join-Path $install 'Settings\Atrium.Settings.exe') 'Settings' $settingsArguments
    foreach ($process in $started) {
        if ($process.HasExited) { throw '新版程序在启动确认期间退出。' }
    }
    # With the core intentionally off, only settings is launched and verified.
    $confirmed = $true
    Remove-Item -LiteralPath $StatePath -Force
    Write-Result ''
} catch {
    $failure = $_.Exception.Message
    if ($backupStarted -and !$confirmed) {
        $rollbackErrors = New-Object 'System.Collections.Generic.List[string]'
        if ($incomingStarted) {
            try {
                $newCore = Join-Path $install 'Atrium.exe'
                if ($StartCore -eq '1' -and (Test-Path -LiteralPath $newCore)) {
                    $quit = Start-Process -FilePath $newCore -ArgumentList '--exit' -WorkingDirectory $workingDirectory -WindowStyle Hidden -PassThru
                    try { [void]$quit.WaitForExit(8000) } finally { $quit.Dispose() }
                }
            } catch { }
            try { Stop-InstallProcesses -WaitMilliseconds 3000 -Tracked @($started) } catch { }
            try {
                Assert-TemporaryPath $incoming
                if (Test-Path -LiteralPath $install) { Move-Tree $install $incoming -Keep -BestEffort -Errors $rollbackErrors }
            } catch { $rollbackErrors.Add($_.Exception.Message) }
        }
        try {
            Assert-TemporaryPath $backup
            if (Test-Path -LiteralPath $backup) { Move-Tree $backup $install -BestEffort -Errors $rollbackErrors }
        } catch { $rollbackErrors.Add($_.Exception.Message) }
        $restored = $true
        if ($rollbackErrors.Count -gt 0 -or ((Test-Path -LiteralPath $backup) -and @(Get-ChildItem -LiteralPath $backup -Force).Count -gt 0)) {
            $detail = (($rollbackErrors | Select-Object -Unique) -join ' ')
            if ($detail) { $failure += " 恢复旧包未完成：$detail" }
            $failure += "；恢复副本保留在 $backup。"
        }
    }
    Write-Result $failure
} finally {
    # Never delete an unconfirmed recovery point, even when rollback itself failed.
    $cleanup = @($incoming)
    if ($confirmed -or ((Test-Path -LiteralPath $backup) -and @(Get-ChildItem -LiteralPath $backup -Force).Count -eq 0)) { $cleanup += $backup }
    foreach ($temporary in $cleanup) {
        Assert-TemporaryPath $temporary
        if (Test-Path -LiteralPath $temporary) {
            try { Remove-Item -LiteralPath $temporary -Recurse -Force } catch { Write-Warning $_.Exception.Message }
        }
    }
    if (!$confirmed -and (!$backupStarted -or $restored)) {
        $restoredCore = Join-Path $install 'Atrium.exe'; $backupCore = Join-Path $backup 'Atrium.exe'
        if ($coreStopped -and $StartCore -eq '1' -and (Test-Path -LiteralPath $restoredCore) -and !(Test-Path -LiteralPath $backupCore)) {
            try { Start-Process -FilePath $restoredCore -WorkingDirectory $workingDirectory -WindowStyle Hidden } catch { Write-Warning $_.Exception.Message }
        }
        $restoredSettings = Join-Path $install 'Settings\Atrium.Settings.exe'
        $backupSettings = Join-Path $backup 'Settings\Atrium.Settings.exe'
        $backupSettingsDll = Join-Path $backup 'Settings\Atrium.Settings.dll'
        if ((Test-Path -LiteralPath $restoredSettings) -and !(Test-Path -LiteralPath $backupSettings) -and !(Test-Path -LiteralPath $backupSettingsDll)) {
            try { Start-Process -FilePath $restoredSettings -ArgumentList '--page about' -WorkingDirectory $workingDirectory -WindowStyle Hidden }
            catch { Write-Warning $_.Exception.Message }
        }
    }
    if ($confirmed) {
        $work = $package
        while ($work.StartsWith($cache + '\', [StringComparison]::OrdinalIgnoreCase) -and (Split-Path -Parent $work) -ne $cache) {
            $work = Split-Path -Parent $work
        }
        $absoluteWork = [IO.Path]::GetFullPath($work)
        if ((Split-Path -Parent $absoluteWork) -eq $cache -and (Split-Path -Leaf $absoluteWork) -match '^package-(background|[0-9a-f]{32})$') {
            try { Remove-Item -LiteralPath $absoluteWork -Recurse -Force } catch { Write-Warning $_.Exception.Message }
        }
    }
    foreach ($process in $started) { $process.Dispose() }
}
