$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$sourceUpdate = Join-Path $project 'tools\Update.ps1'
$ps51 = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$csc = Join-Path $env:SystemRoot 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$fixtureRoot = Join-Path ([IO.Path]::GetFullPath([IO.Path]::GetTempPath())) ('Atrium-update-tests-' + [Guid]::NewGuid().ToString('N'))
$stubSourcePath = Join-Path $fixtureRoot 'Stub.cs'
$stubExe = Join-Path $fixtureRoot 'stub.exe'
function Populate-App([string]$Dir, [hashtable]$Mode) {
    $sDir = Join-Path $Dir 'Settings'; New-Item -ItemType Directory -Path $sDir -Force | Out-Null
    Copy-Item -LiteralPath $stubExe -Destination (Join-Path $Dir 'Atrium.exe') -Force
    Copy-Item -LiteralPath $stubExe -Destination (Join-Path $sDir 'Atrium.Settings.exe') -Force
    $lines = foreach ($k in $Mode.Keys) { "$k=$($Mode[$k])" }
    Set-Content -LiteralPath (Join-Path $Dir 'mode.txt') -Value $lines -Encoding ASCII
    Set-Content -LiteralPath (Join-Path $sDir 'mode.txt') -Value $lines -Encoding ASCII
}
function Wait-Log([string]$LogPath, [string]$Pattern) {
    $until = [DateTime]::UtcNow.AddSeconds(5)
    while ([DateTime]::UtcNow -lt $until) {
        if ((Test-Path -LiteralPath $LogPath) -and ((Get-Content -LiteralPath $LogPath -Raw) -match $Pattern)) { return }
        Start-Sleep -Milliseconds 50
    }
    throw "Timed out waiting for log pattern: $Pattern"
}
function Stop-CaseProcesses([string]$CaseDir, [System.Diagnostics.Process[]]$Extra = @()) {
    foreach ($p in $Extra) {
        try { if ($p -and !$p.HasExited) { $p.Kill(); [void]$p.WaitForExit(2000) }; if ($p) { $p.Dispose() } } catch { }
    }
    $prefix = $CaseDir.TrimEnd('\') + '\'
    foreach ($p in @(Get-Process -EA SilentlyContinue)) {
        try {
            if ($p.Path -and $p.Path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
                Stop-Process -Id $p.Id -Force -EA SilentlyContinue; [void]$p.WaitForExit(2000)
            }
        } catch { }
    }
    try {
        foreach ($cim in @(Get-CimInstance -ClassName Win32_Process -ErrorAction Stop)) {
            if ($cim.CommandLine -and $cim.CommandLine.IndexOf($CaseDir, [StringComparison]::OrdinalIgnoreCase) -ge 0 -and [int]$cim.ProcessId -ne $PID) {
                $procId = [int]$cim.ProcessId
                $target = Get-Process -Id $procId -ErrorAction SilentlyContinue
                Stop-Process -Id $procId -Force -ErrorAction SilentlyContinue
                if ($target) { [void]$target.WaitForExit(2000); $target.Dispose() }
            }
        }
    } catch { }
}
function New-CaseLayout([string]$Name) {
    $caseDir = Join-Path $fixtureRoot $Name; $install = Join-Path $caseDir 'Atrium'
    $cache = Join-Path $caseDir 'cache'; $pkg = Join-Path $cache 'package-background'
    New-Item -ItemType Directory -Path $install, $pkg | Out-Null
    $scriptCopy = Join-Path $cache 'Update.ps1'; Copy-Item -LiteralPath $sourceUpdate -Destination $scriptCopy -Force
    $statePath = Join-Path $cache 'ready.json'; '{}' | Set-Content -LiteralPath $statePath -Encoding UTF8
    return @{ Dir = $caseDir; Install = $install; Package = $pkg; Script = $scriptCopy; State = $statePath
        Result = (Join-Path $cache 'result.json'); Log = (Join-Path $caseDir 'events.log'); Exit = (Join-Path $caseDir 'exit.sig') }
}
function Invoke-Updater($C, [int]$WaitPid = 0) {
    if ($WaitPid -le 0) { $d = Start-Process -FilePath $stubExe -ArgumentList '--sleep 10' -WindowStyle Hidden -PassThru; $WaitPid = $d.Id; $d.Dispose() }
    $args = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $C.Script, '-InstallDirectory', $C.Install, '-PackageDirectory', $C.Package,
        '-StatePath', $C.State, '-WaitForProcessId', $WaitPid, '-StartCore', '1', '-StartupTimeoutSeconds', '4')
    $proc = Start-Process -FilePath $ps51 -ArgumentList $args -WindowStyle Hidden -PassThru
    try {
        if (!$proc.WaitForExit(45000)) {
            Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue; [void]$proc.WaitForExit(3000)
            throw "Update.ps1 timed out after 45s in $($C.Dir)"
        }
        $script:updaterExitCode = $proc.ExitCode
    } finally { $proc.Dispose() }
}
function Run-RollbackCase($c, [System.Collections.ArrayList]$tracked) {
    [void]$tracked.Add((Start-Process -FilePath (Join-Path $c.Install 'Atrium.exe') -WindowStyle Hidden -PassThru))
    Wait-Log $c.Log 'v1:Atrium\.exe:start'
    Invoke-Updater $c
    return (Get-Content -LiteralPath $c.Result -Raw -Encoding UTF8 | ConvertFrom-Json)
}
function Assert-V1Restarted($c) { Wait-Log $c.Log '(?s)v1:Atrium\.exe:start.*v1:Atrium\.exe:start'; Wait-Log $c.Log 'v1:Atrium\.Settings\.exe:start' }
try {
    New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
    $stubSource = @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Threading;
static class Stub {
    static Dictionary<string, string> ReadMode(string dir) {
        var m = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        string p = Path.Combine(dir, "mode.txt");
        if (!File.Exists(p)) return m;
        foreach (string line in File.ReadAllLines(p)) { int i = line.IndexOf('='); if (i > 0) m[line.Substring(0, i).Trim()] = line.Substring(i + 1).Trim(); }
        return m;
    }
    static string Get(Dictionary<string, string> m, string k) { string v; return m.TryGetValue(k, out v) ? v : ""; }
    static void Log(Dictionary<string, string> m, string msg) {
        string p = Get(m, "events_log"); if (p.Length == 0) return;
        for (int i = 0; i < 20; i++) { try { File.AppendAllText(p, msg + Environment.NewLine); return; } catch { Thread.Sleep(20); } }
    }
    static bool ShouldExit(bool respectExit, string exitFile, bool exitExisted, DateTime startWrite) {
        return respectExit && exitFile.Length > 0 && File.Exists(exitFile) &&
            (!exitExisted || File.GetLastWriteTimeUtc(exitFile) > startWrite);
    }
    static void WriteReady(string path) {
        if (path.Length == 0) return;
        Directory.CreateDirectory(Path.GetDirectoryName(path));
        File.WriteAllText(path, "1");
    }
    static bool SpawnChildAndWait(Dictionary<string, string> m, string exePath, string args, string tag) {
        string logDir = Path.GetDirectoryName(Get(m, "events_log"));
        string readyDir = string.IsNullOrEmpty(logDir) ? Path.GetTempPath() : logDir;
        string readyPath = Path.Combine(readyDir, tag + "-" + Guid.NewGuid().ToString("N") + ".ready");
        using (Process p = Process.Start(new ProcessStartInfo(exePath, args + " --lock-ready \"" + readyPath + "\"") { UseShellExecute = false })) {
            for (int i = 0; i < 100 && !File.Exists(readyPath); i++) {
                if (p != null && p.HasExited) return false;
                Thread.Sleep(50);
            }
        }
        return File.Exists(readyPath);
    }
    static int Main(string[] args) {
        string dir = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location), exe = Path.GetFileName(Assembly.GetExecutingAssembly().Location);
        var m = ReadMode(dir);
        string ver = Get(m, "version"), readyEv = "", lockP = "", lockR = ""; int sleepMs = 0; bool isExit = false, isGuard = false, isResume = false;
        for (int i = 0; i < args.Length; i++) {
            if (args[i] == "--ready-event" && i + 1 < args.Length) readyEv = args[++i];
            else if (args[i] == "--lock" && i + 1 < args.Length) lockP = args[++i];
            else if (args[i] == "--lock-ready" && i + 1 < args.Length) lockR = args[++i];
            else if (args[i] == "--sleep" && i + 1 < args.Length) int.TryParse(args[++i], out sleepMs);
            else if (args[i] == "--exit") isExit = true; else if (args[i] == "--guard") isGuard = true; else if (args[i] == "--resume-update") isResume = true;
        }
        if (sleepMs > 0) { Thread.Sleep(sleepMs); return 0; }
        if (lockP.Length > 0) {
            Directory.CreateDirectory(Path.GetDirectoryName(lockP));
            using (new FileStream(lockP, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None)) {
                WriteReady(lockR); Thread.Sleep(60000);
            } return 0;
        }
        string exitFile = Get(m, "exit_signal");
        if (isExit) { if (exitFile.Length > 0) File.WriteAllText(exitFile, "1"); Log(m, ver + ":" + exe + ":exit"); Thread.Sleep(150); return 0; }
        bool respectExit = Get(m, "respect_exit") == "1";
        bool exitExisted = exitFile.Length > 0 && File.Exists(exitFile);
        DateTime startWrite = exitExisted ? File.GetLastWriteTimeUtc(exitFile) : DateTime.MinValue;
        if (isGuard || isResume) {
            string rel = Get(m, isGuard ? "guard_lock" : "headless_lock"); FileStream fs = null;
            if (rel.Length > 0) {
                string full = Path.Combine(isResume ? Path.GetDirectoryName(dir) : dir, rel);
                Directory.CreateDirectory(Path.GetDirectoryName(full));
                fs = new FileStream(full, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
            }
            try {
                Log(m, ver + ":" + exe + ":" + (isGuard ? "--guard" : "--resume-update") + ":start");
                WriteReady(lockR);
                int holdMs = 0; if (isResume) int.TryParse(Get(m, "headless_hold_ms"), out holdMs);
                for (int e = 0; e < 60000; e += 50) {
                    if ((holdMs > 0 && e >= holdMs) || (holdMs == 0 && ShouldExit(respectExit, exitFile, exitExisted, startWrite))) break;
                    Thread.Sleep(50);
                }
            } finally { if (fs != null) fs.Dispose(); }
            return 0;
        }
        Log(m, ver + ":" + exe + ":start");
        if (exe.Equals("Atrium.Settings.exe", StringComparison.OrdinalIgnoreCase)) {
            string spawnExe = Get(m, "spawn_lock_exe");
            if (spawnExe.Length > 0) {
                string lr = Get(m, "spawn_lock_ready");
                Process.Start(new ProcessStartInfo(spawnExe,
                    "--lock \"" + Get(m, "spawn_lock_target") + "\" --lock-ready \"" + lr + "\"") { UseShellExecute = false });
                for (int i = 0; i < 60 && !File.Exists(lr); i++) Thread.Sleep(50);
            }
            int code = 0; int.TryParse(Get(m, "settings_exit_code"), out code);
            if (code != 0) { Log(m, ver + ":" + exe + ":fail"); return code; }
        }
        if (exe.Equals("Atrium.exe", StringComparison.OrdinalIgnoreCase)) {
            string self = Assembly.GetExecutingAssembly().Location;
            if (Get(m, "spawn_guard") == "1" && !SpawnChildAndWait(m, self, "--guard " + Process.GetCurrentProcess().Id, "guard")) return 2;
            if (Get(m, "spawn_headless") == "1" &&
                !SpawnChildAndWait(m, Path.Combine(dir, "Settings\\Atrium.Settings.exe"), "--resume-update", "headless")) return 2;
        }
        if (readyEv.Length > 0) using (var ev = EventWaitHandle.OpenExisting(readyEv)) ev.Set();
        for (int e = 0; e < 60000; e += 50) { if (ShouldExit(respectExit, exitFile, exitExisted, startWrite)) break; Thread.Sleep(50); }
        return 0;
    }
}
'@
    Set-Content -LiteralPath $stubSourcePath -Value $stubSource -Encoding UTF8
    & $csc /nologo /target:winexe /out:$stubExe $stubSourcePath
    if ($LASTEXITCODE -ne 0) { throw 'Failed to compile stub.exe.' }
    # Case 1: Normal update with headless workers and Hidden+ReadOnly files
    $c = New-CaseLayout 'case1-success'; $tracked = New-Object System.Collections.ArrayList
    try {
        Populate-App $c.Install @{ version = 'v1'; respect_exit = '1'; exit_signal = $c.Exit; events_log = $c.Log
            spawn_guard = '1'; spawn_headless = '1'; guard_lock = 'Settings\guard.lock'; headless_lock = 'Settings\headless.lock'; headless_hold_ms = '1800' }
        Populate-App $c.Package @{ version = 'v2'; respect_exit = '1'; exit_signal = "$($c.Dir)\exit-v2.sig"; events_log = $c.Log }
        Set-Content -LiteralPath (Join-Path $c.Install 'development.txt') -Value 'dev' -Encoding UTF8
        $hiddenOld = Join-Path $c.Install 'secret.dat'; Set-Content -LiteralPath $hiddenOld -Value 'v1-secret' -Encoding UTF8
        (Get-Item -LiteralPath $hiddenOld).Attributes = 'Hidden, ReadOnly'
        Set-Content -LiteralPath (Join-Path $c.Package 'secret.dat') -Value 'v2-secret' -Encoding UTF8
        [void]$tracked.Add((Start-Process -FilePath (Join-Path $c.Install 'Atrium.exe') -WindowStyle Hidden -PassThru))
        Wait-Log $c.Log 'v1:Atrium\.exe:--guard:start'; Wait-Log $c.Log 'v1:Atrium\.Settings\.exe:--resume-update:start'
        $uiProc = Start-Process -FilePath $stubExe -ArgumentList '--sleep 400' -WindowStyle Hidden -PassThru; [void]$tracked.Add($uiProc)
        Invoke-Updater $c $uiProc.Id
        if ($script:updaterExitCode -ne 0) { throw "Case 1 failed with exit code $script:updaterExitCode" }
        $res = Get-Content -LiteralPath $c.Result -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($res.error -ne '' -or (Test-Path -LiteralPath $c.State) -or (Test-Path -LiteralPath $c.Package)) { throw "Case 1 failed: $($res.error)" }
        if (!(Test-Path -LiteralPath (Join-Path $c.Install 'development.txt')) -or
            (Get-Content -LiteralPath $hiddenOld -Raw).Trim() -ne 'v2-secret') { throw 'Case 1: file mismatch' }
        Wait-Log $c.Log 'v2:Atrium\.exe:start'; Wait-Log $c.Log 'v2:Atrium\.Settings\.exe:start'
    } finally { Stop-CaseProcesses $c.Dir $tracked.ToArray() }
    # Case 2: New Settings fails with --guard and untracked child holding locks (P0-4)
    $c = New-CaseLayout 'case2-rollback-locks'; $tracked = New-Object System.Collections.ArrayList
    try {
        Populate-App $c.Install @{ version = 'v1'; respect_exit = '1'; exit_signal = $c.Exit; events_log = $c.Log }
        Populate-App $c.Package @{ version = 'v2'; respect_exit = '0'; exit_signal = "$($c.Dir)\exit-v2.sig"; events_log = $c.Log
            spawn_guard = '1'; spawn_headless = '1'; guard_lock = 'Settings\guard.lock'; headless_lock = 'Settings\headless.lock'; settings_exit_code = '1' }
        $hiddenOld = Join-Path $c.Install 'secret.dat'; Set-Content -LiteralPath $hiddenOld -Value 'v1-secret' -Encoding UTF8
        (Get-Item -LiteralPath $hiddenOld).Attributes = 'Hidden, ReadOnly'
        Set-Content -LiteralPath (Join-Path $c.Package 'secret.dat') -Value 'v2-secret' -Encoding UTF8
        $res = Run-RollbackCase $c $tracked
        if (!$res.error -or !(Test-Path -LiteralPath $c.State) -or
            (Get-Content -LiteralPath $hiddenOld -Raw).Trim() -ne 'v1-secret') { throw 'Case 2: rollback mismatch' }
        if (@(Get-ChildItem -LiteralPath $c.Dir -Directory -Filter 'Atrium.*-*').Count -ne 0) { throw 'Case 2: temp folders remained' }
        Wait-Log $c.Log 'v2:Atrium\.exe:--guard:start'
        Wait-Log $c.Log 'v2:Atrium\.Settings\.exe:--resume-update:start'
        Assert-V1Restarted $c
    } finally { Stop-CaseProcesses $c.Dir $tracked.ToArray() }
    # Case 3: External process locks a file during rollback -> partial rollback resilience
    $c = New-CaseLayout 'case3-partial-rollback'; $tracked = New-Object System.Collections.ArrayList
    try {
        $extra = Join-Path $c.Install 'extra.dll'
        Populate-App $c.Install @{ version = 'v1'; respect_exit = '1'; exit_signal = $c.Exit; events_log = $c.Log }
        Populate-App $c.Package @{ version = 'v2'; respect_exit = '1'; exit_signal = "$($c.Dir)\exit-v2.sig"; events_log = $c.Log
            settings_exit_code = '1'; spawn_lock_exe = $stubExe; spawn_lock_target = $extra; spawn_lock_ready = (Join-Path $c.Dir 'lock.ready') }
        Set-Content -LiteralPath $extra -Value 'v1-extra' -Encoding UTF8
        Set-Content -LiteralPath (Join-Path $c.Package 'extra.dll') -Value 'v2-extra' -Encoding UTF8
        $res = Run-RollbackCase $c $tracked
        $backupDirs = @(Get-ChildItem -LiteralPath $c.Dir -Directory -Filter 'Atrium.previous-*')
        if ($backupDirs.Count -ne 1 -or
            !(Test-Path -LiteralPath (Join-Path $backupDirs[0].FullName 'extra.dll'))) { throw 'Case 3: expected extra.dll in backup' }
        if ($res.error -notmatch [regex]::Escape($backupDirs[0].FullName)) { throw "Case 3: error should mention backup path, got: $($res.error)" }
        Assert-V1Restarted $c
    } finally { Stop-CaseProcesses $c.Dir $tracked.ToArray() }
    # Case 4: Stage 1 backup failure must not lose user files
    $c = New-CaseLayout 'case4-stage1-fail'; $tracked = New-Object System.Collections.ArrayList
    try {
        Populate-App $c.Install @{ version = 'v1'; respect_exit = '1'; exit_signal = $c.Exit; events_log = $c.Log }
        Populate-App $c.Package @{ version = 'v2'; respect_exit = '1'; exit_signal = "$($c.Dir)\exit-v2.sig"; events_log = $c.Log }
        $userFile = Join-Path $c.Install 'user-file.txt'; Set-Content -LiteralPath $userFile -Value 'keep-me' -Encoding UTF8
        $lockReady = Join-Path $c.Dir 'lock.ready'
        $lockProc = Start-Process -FilePath $stubExe -ArgumentList "--lock `"$userFile`" --lock-ready `"$lockReady`"" -WindowStyle Hidden -PassThru
        [void]$tracked.Add($lockProc)
        $until = [DateTime]::UtcNow.AddSeconds(5)
        while (!(Test-Path -LiteralPath $lockReady) -and [DateTime]::UtcNow -lt $until) { Start-Sleep -Milliseconds 30 }
        if (!(Test-Path -LiteralPath $lockReady)) { throw 'Case 4: lock helper did not become ready in time' }
        $res = Run-RollbackCase $c $tracked
        if (!$res.error -or !(Test-Path -LiteralPath $userFile)) { throw 'Case 4: missing error or user-file.txt lost' }
        Stop-Process -Id $lockProc.Id -Force -ErrorAction SilentlyContinue; [void]$lockProc.WaitForExit(2000)
        if ((Get-Content -LiteralPath $userFile -Raw).Trim() -ne 'keep-me') { throw 'Case 4: user-file.txt modified' }
        if (!(Test-Path -LiteralPath (Join-Path $c.Install 'Atrium.exe')) -or
            !(Test-Path -LiteralPath (Join-Path $c.Install 'Settings\Atrium.Settings.exe'))) { throw 'Case 4: v1 binaries missing' }
        if (@(Get-ChildItem -LiteralPath $c.Dir -Directory -Filter 'Atrium.*-*').Count -ne 0) { throw 'Case 4: temp folders remained' }
        Assert-V1Restarted $c
    } finally { Stop-CaseProcesses $c.Dir $tracked.ToArray() }
    Write-Host 'Updater regressions passed (4 cases).'
} finally {
    Stop-CaseProcesses $fixtureRoot
    if (Test-Path -LiteralPath $fixtureRoot) { Remove-Item -LiteralPath $fixtureRoot -Recurse -Force -ErrorAction SilentlyContinue }
}
exit 0
