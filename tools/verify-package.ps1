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
        'Settings\Update.ps1', 'Uninstall.ps1', 'LICENSE', 'locales\manifest.json', 'package-manifest.json')
    foreach ($rel in $required) {
        if (!(Test-Path -LiteralPath (Join-Path $pkg $rel) -PathType Leaf)) { throw "Missing package file: $rel" }
    }
    $pkgFull = [IO.Path]::GetFullPath($pkg).TrimEnd('\')
    $pkgManifest = Get-Content -LiteralPath (Join-Path $pkgFull 'package-manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($null -eq $pkgManifest -or (($pkgManifest.schema -isnot [int]) -and ($pkgManifest.schema -isnot [long])) -or
        $pkgManifest.schema -ne 1) { throw 'Invalid package-manifest.json schema.' }
    if ($pkgManifest.version -ne $expectedVersion) { throw "package-manifest.json version '$($pkgManifest.version)' != $expectedVersion." }
    $manifestFiles = @($pkgManifest.files)
    if ($manifestFiles.Count -eq 0) { throw 'package-manifest.json files list is empty.' }
    $listed = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        foreach ($entry in $manifestFiles) {
            $rel = [string]$entry.path
            if ([string]::IsNullOrWhiteSpace($rel) -or [IO.Path]::IsPathRooted($rel) -or
                $rel.StartsWith('\') -or $rel.StartsWith('/') -or $rel.EndsWith('\') -or $rel.EndsWith('/')) {
                throw "Invalid manifest path: '$rel'"
            }
            $norm = $rel.Replace('/', '\')
            if ($norm.Split('\') | Where-Object { $_ -eq '' -or $_ -eq '.' -or $_ -eq '..' }) {
                throw "Unsafe manifest path: '$rel'"
            }
            $full = [IO.Path]::GetFullPath((Join-Path $pkgFull $norm))
            if (!$full.StartsWith($pkgFull + '\', [StringComparison]::OrdinalIgnoreCase)) {
                throw "Out-of-bounds manifest path: '$rel'"
            }
            if (!$listed.Add($norm)) { throw "Duplicate manifest path: '$rel'" }
            if (!(Test-Path -LiteralPath $full -PathType Leaf)) { throw "Missing file listed in package-manifest.json: $norm" }
            $expectedHash = [string]$entry.sha256
            if ($expectedHash -cnotmatch '^[0-9a-f]{64}$') { throw "Invalid SHA-256 in manifest for $norm" }
            $stream = [IO.File]::OpenRead($full)
            try { $actualHash = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
            finally { $stream.Dispose() }
            if ($actualHash -cne $expectedHash) { throw "SHA-256 mismatch for $norm" }
        }
    } finally {
        $sha.Dispose()
    }
    foreach ($file in @(Get-ChildItem -LiteralPath $pkgFull -Recurse -File -Force)) {
        $rel = $file.FullName.Substring($pkgFull.Length + 1).Replace('/', '\')
        if ($rel -ieq 'package-manifest.json' -or $rel -ieq 'development.txt') { continue }
        if (!$listed.Contains($rel)) { throw "File not listed in package-manifest.json: $rel" }
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
        function Read-LogDelta([string]$path, [long]$initialLen) {
            if (!(Test-Path -LiteralPath $path -PathType Leaf)) { return '(missing)' }
            $fs = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
            try {
                $offset = if ($fs.Length -lt $initialLen) { 0L } else { $initialLen }
                [void]$fs.Seek($offset, [IO.SeekOrigin]::Begin)
                $sr = New-Object IO.StreamReader($fs, [Text.Encoding]::UTF8, $true)
                try { $sr.ReadToEnd() } finally { $sr.Dispose() }
            } finally {
                $fs.Dispose()
            }
        }
        function Wait-Until([int]$timeoutMs, [scriptblock]$cond, [string]$failMessage) {
            $sw = [Diagnostics.Stopwatch]::StartNew()
            while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
                if (& $cond) { return }
                Start-Sleep -Milliseconds 100
            }
            if (& $cond) { return }
            throw $failMessage
        }
        $running = @(Get-Process -Name Atrium -ErrorAction SilentlyContinue)
        if ($running.Count -gt 0) {
            throw "Atrium is already running (PID: $($running.Id -join ', ')); stop it before -SmokeStart."
        }
        $runningSettings = @(Get-Process -Name 'Atrium.Settings' -ErrorAction SilentlyContinue)
        if ($runningSettings.Count -gt 0) {
            throw "Atrium.Settings is already running (PID: $($runningSettings.Id -join ', ')); stop it before -SmokeStart."
        }
        if (!('AtriumVerify.UiaSmoke' -as [type])) {
            Add-Type -Language CSharp -TypeDefinition @'
#nullable enable
using System;
using System.Runtime.InteropServices;
using System.Threading;

namespace AtriumVerify
{
    public static class UiaSmoke
    {
        private const int TreeScopeDescendants = 4;
        private const int TogglePatternId = 10015;
        private const int InvokePatternId = 10000;
        private const int AutomationIdPropId = 30011;
        private const int IsInvokeAvailablePropId = 30031;
        private const int IsToggleAvailablePropId = 30041;
        private const uint WmClose = 0x0010;

        private delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lparam);
        [DllImport("user32.dll")] private static extern bool EnumWindows(EnumWindowsProc proc, IntPtr lparam);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr GetProp(IntPtr hwnd, string name);
        [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
        [DllImport("user32.dll")] private static extern bool PostMessage(IntPtr hwnd, uint msg, IntPtr wparam, IntPtr lparam);

        [ComImport, Guid("352ffba8-0973-437c-a61f-f64cafd81df9"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IUIAutomationCondition { }

        [ComImport, Guid("fb377fbe-8ea6-46d5-9c73-6499642d3059"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IUIAutomationInvokePattern { void Invoke(); }

        [ComImport, Guid("94cf8058-9b8d-4ab9-8bfd-4cd0a33c8c70"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IUIAutomationTogglePattern { void Toggle(); int CurrentToggleState { get; } }

        [ComImport, Guid("d22108aa-8ac5-49a5-837b-37bbb3d7591e"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IUIAutomationElement
        {
            void _0(); void _1();
            [return: MarshalAs(UnmanagedType.Interface)]
            IUIAutomationElement? FindFirst(int scope, IUIAutomationCondition condition);
            void _3(); void _4(); void _5(); void _6(); void _7(); void _8(); void _9(); void _10();
            void _11(); void _12();
            [return: MarshalAs(UnmanagedType.IUnknown)]
            object? GetCurrentPattern(int patternId);
            void _14(); void _15(); void _16(); void _17(); void _18(); void _19(); void _20();
            void _21(); void _22(); void _23(); void _24();
            int CurrentIsEnabled { get; }
        }

        [ComImport, Guid("30cbe57d-d9d0-452a-ab13-7ac5ac4825ee"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IUIAutomation
        {
            void _0(); void _1(); void _2();
            [return: MarshalAs(UnmanagedType.Interface)]
            IUIAutomationElement? ElementFromHandle(IntPtr hwnd);
            void _4(); void _5(); void _6(); void _7(); void _8(); void _9(); void _10();
            void _11(); void _12(); void _13(); void _14(); void _15(); void _16(); void _17(); void _18(); void _19();
            [return: MarshalAs(UnmanagedType.Interface)]
            IUIAutomationCondition CreatePropertyCondition(int propertyId, [MarshalAs(UnmanagedType.Struct)] object value);
            void _21();
            [return: MarshalAs(UnmanagedType.Interface)]
            IUIAutomationCondition CreateAndCondition(IUIAutomationCondition c1, IUIAutomationCondition c2);
        }

        public static IntPtr FindWindowByProp(int pid, string propName)
        {
            IntPtr found = IntPtr.Zero;
            EnumWindows((hwnd, lparam) =>
            {
                if (GetProp(hwnd, propName) == IntPtr.Zero) return true;
                _ = GetWindowThreadProcessId(hwnd, out uint wpid);
                if (wpid != (uint)pid) return true;
                found = hwnd;
                return false;
            }, IntPtr.Zero);
            return found;
        }

        public static bool PostClose(IntPtr hwnd) => PostMessage(hwnd, WmClose, IntPtr.Zero, IntPtr.Zero);

        private static T RunMta<T>(Func<IUIAutomation, T> work, T onComError)
        {
            T result = onComError;
            Exception? error = null;
            var t = new Thread(() =>
            {
                IUIAutomation? uia = null;
                try
                {
                    var clsid = new Guid("E22AD333-B25F-460C-83D0-0581107395C9");
                    uia = (IUIAutomation)Activator.CreateInstance(Type.GetTypeFromCLSID(clsid)!)!;
                    result = work(uia);
                }
                catch (COMException) { result = onComError; }
                catch (Exception ex) { error = ex; }
                finally { if (uia != null && Marshal.IsComObject(uia)) Marshal.ReleaseComObject(uia); }
            });
            t.SetApartmentState(ApartmentState.MTA);
            t.Start();
            t.Join();
            if (error != null) throw new AggregateException(error);
            return result;
        }

        private static IUIAutomationElement? FindById(IUIAutomation uia, IntPtr hwnd, string id, int patternPropId)
        {
            var root = uia.ElementFromHandle(hwnd);
            if (root == null) return null;
            var c1 = uia.CreatePropertyCondition(AutomationIdPropId, id);
            var c2 = uia.CreatePropertyCondition(patternPropId, true);
            var and = uia.CreateAndCondition(c1, c2);
            return root.FindFirst(TreeScopeDescendants, and);
        }

        public static int GetToggleState(IntPtr hwnd, string id) => RunMta(uia =>
        {
            var el = FindById(uia, hwnd, id, IsToggleAvailablePropId);
            if (el == null) return -1;
            var pat = el.GetCurrentPattern(TogglePatternId) as IUIAutomationTogglePattern;
            return pat == null ? -1 : pat.CurrentToggleState;
        }, -1);

        public static bool Toggle(IntPtr hwnd, string id) => RunMta(uia =>
        {
            var el = FindById(uia, hwnd, id, IsToggleAvailablePropId);
            var pat = el?.GetCurrentPattern(TogglePatternId) as IUIAutomationTogglePattern;
            if (pat == null) return false;
            pat.Toggle();
            return true;
        }, false);

        public static bool IsInvokeEnabled(IntPtr hwnd, string id) => RunMta(uia =>
        {
            var el = FindById(uia, hwnd, id, IsInvokeAvailablePropId);
            return el != null && el.CurrentIsEnabled != 0;
        }, false);

        public static bool Invoke(IntPtr hwnd, string id) => RunMta(uia =>
        {
            var el = FindById(uia, hwnd, id, IsInvokeAvailablePropId);
            var pat = el?.GetCurrentPattern(InvokePatternId) as IUIAutomationInvokePattern;
            if (pat == null) return false;
            pat.Invoke();
            return true;
        }, false);
    }
}
'@
        }
        $appDataDir = Join-Path $env:LOCALAPPDATA 'Atrium'
        $settingsPath = Join-Path $appDataDir 'settings.json'
        $hadSettings = Test-Path -LiteralPath $settingsPath -PathType Leaf
        $savedSettingsBytes = if ($hadSettings) { [IO.File]::ReadAllBytes($settingsPath) } else { $null }
        if ($hadSettings) { Remove-Item -LiteralPath $settingsPath -Force }
        $logPath = Join-Path $appDataDir 'logs\core.log'
        $initialLogLen = if (Test-Path -LiteralPath $logPath -PathType Leaf) { (Get-Item -LiteralPath $logPath).Length } else { 0L }
        $settingsLogPath = Join-Path $appDataDir 'logs\settings.log'
        $initialSettingsLogLen = if (Test-Path -LiteralPath $settingsLogPath -PathType Leaf) { (Get-Item -LiteralPath $settingsLogPath).Length } else { 0L }
        $core = Join-Path $pkg 'Atrium.exe'
        $settingsExe = Join-Path $pkg 'Settings\Atrium.Settings.exe'
        $eventName = 'Local\Atrium.Update.Core.' + [Guid]::NewGuid().ToString('N')
        $ready = New-Object System.Threading.EventWaitHandle($false, [Threading.EventResetMode]::ManualReset, $eventName)
        $proc = $null
        $settingsProcs = [System.Collections.Generic.List[Diagnostics.Process]]::new()
        $settingsWaitHandles = [System.Collections.Generic.List[System.Threading.EventWaitHandle]]::new()
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
                if ($state.op -ne 'state' -or [int]$state.protocol -ne 1 -or
                    [string]::IsNullOrWhiteSpace([string]$state.instanceId) -or [int64]$state.revision -lt 1) {
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
                $expectedInstanceId = [string]$state.instanceId
                $expectedRev = [int64]$state.revision
                if ([double]$after.current.height -ne $savedHeight -or
                    [string]$after.instanceId -ne $expectedInstanceId -or [int64]$after.revision -ne $expectedRev) {
                    throw "Preview lease rollback mismatch: $($after | ConvertTo-Json -Compress)"
                }
                $conflictReq = @{
                    op = 'commit'
                    instanceId = $expectedInstanceId
                    expectedRevision = 0
                    settings = $state.saved
                } | ConvertTo-Json -Depth 16 -Compress
                $conflict = Send-PipeRequest $pipe2 $conflictReq
                if ($conflict.op -ne 'conflict' -or [string]$conflict.instanceId -ne $expectedInstanceId -or
                    [int64]$conflict.revision -ne $expectedRev -or $null -eq $conflict.saved) {
                    throw "Expected conflict at revision ${expectedRev}: $($conflict | ConvertTo-Json -Compress)"
                }
                $commitReq = @{
                    op = 'commit'
                    instanceId = $expectedInstanceId
                    expectedRevision = $expectedRev
                    settings = $state.saved
                } | ConvertTo-Json -Depth 16 -Compress
                $commitReply = Send-PipeRequest $pipe2 $commitReq
                if ($commitReply.op -ne 'ok' -or [string]$commitReply.instanceId -ne $expectedInstanceId -or
                    [int64]$commitReply.revision -ne ($expectedRev + 1)) {
                    throw "Expected commit ok at revision $($expectedRev + 1): $($commitReply | ConvertTo-Json -Compress)"
                }
            } finally {
                $pipe2.Dispose()
            }
            $sEvent1 = 'Local\Atrium.Update.Settings.' + [Guid]::NewGuid().ToString('N')
            $sReady1 = New-Object System.Threading.EventWaitHandle($false, [Threading.EventResetMode]::ManualReset, $sEvent1)
            $settingsWaitHandles.Add($sReady1)
            $sArgs1 = "--development --page clock --require-core-ready --ready-event $sEvent1"
            $sProc1 = Start-Process -FilePath $settingsExe -ArgumentList $sArgs1 -WorkingDirectory $temp -PassThru
            $settingsProcs.Add($sProc1)
            if (!$sReady1.WaitOne(20000) -or $sProc1.HasExited) { throw 'Atrium.Settings.exe failed first smoke startup.' }
            $hwnd1 = [IntPtr]::Zero
            Wait-Until 10000 {
                $script:hwndTmp = [AtriumVerify.UiaSmoke]::FindWindowByProp($sProc1.Id, 'Atrium.Settings.Window')
                $script:hwndTmp -ne [IntPtr]::Zero
            } 'Atrium.Settings.exe window handle not found on first launch.'
            $hwnd1 = $script:hwndTmp
            Wait-Until 10000 {
                ([AtriumVerify.UiaSmoke]::GetToggleState($hwnd1, 'ClockSecondsSwitch')) -eq 0
            } 'ClockSecondsSwitch did not report initial Off (0) state via UIA.'
            Wait-Until 10000 {
                [AtriumVerify.UiaSmoke]::Toggle($hwnd1, 'ClockSecondsSwitch')
            } 'Failed to toggle ClockSecondsSwitch via UIA.'
            Wait-Until 10000 {
                ([AtriumVerify.UiaSmoke]::GetToggleState($hwnd1, 'ClockSecondsSwitch')) -eq 1 -and
                [AtriumVerify.UiaSmoke]::IsInvokeEnabled($hwnd1, 'SaveButton')
            } 'ClockSecondsSwitch did not become On (1) with enabled SaveButton.'
            Wait-Until 10000 {
                [AtriumVerify.UiaSmoke]::Invoke($hwnd1, 'SaveButton')
            } 'Failed to invoke SaveButton via UIA.'
            Wait-Until 10000 {
                if (!(Test-Path -LiteralPath $settingsPath -PathType Leaf)) { return $false }
                try {
                    $disk = Get-Content -LiteralPath $settingsPath -Raw -Encoding UTF8 | ConvertFrom-Json
                    return ($disk.showClockSeconds -eq $true) -and ![AtriumVerify.UiaSmoke]::IsInvokeEnabled($hwnd1, 'SaveButton')
                } catch {
                    return $false
                }
            } 'Saved settings.json missing showClockSeconds=true or SaveButton remained enabled.'
            if (![AtriumVerify.UiaSmoke]::PostClose($hwnd1)) { throw 'PostMessage(WM_CLOSE) failed on first Settings window.' }
            if (!$sProc1.WaitForExit(15000) -or $sProc1.ExitCode -ne 0) {
                throw "First Atrium.Settings.exe did not exit cleanly (exited=$($sProc1.HasExited), exitCode=$($sProc1.ExitCode))."
            }
            $pipe3 = New-Object System.IO.Pipes.NamedPipeClientStream('.', $pipeName, [System.IO.Pipes.PipeDirection]::InOut)
            try {
                $pipe3.Connect(5000)
                $pipe3.ReadMode = [System.IO.Pipes.PipeTransmissionMode]::Message
                $afterSave = Send-PipeRequest $pipe3 '{"op":"get"}'
                if ($afterSave.saved.showClockSeconds -ne $true -or $afterSave.current.showClockSeconds -ne $true) {
                    throw "Core pipe get mismatch after UI save: $($afterSave | ConvertTo-Json -Compress)"
                }
            } finally {
                $pipe3.Dispose()
            }
            $sEvent2 = 'Local\Atrium.Update.Settings.' + [Guid]::NewGuid().ToString('N')
            $sReady2 = New-Object System.Threading.EventWaitHandle($false, [Threading.EventResetMode]::ManualReset, $sEvent2)
            $settingsWaitHandles.Add($sReady2)
            $sArgs2 = "--development --page clock --require-core-ready --ready-event $sEvent2"
            $sProc2 = Start-Process -FilePath $settingsExe -ArgumentList $sArgs2 -WorkingDirectory $temp -PassThru
            $settingsProcs.Add($sProc2)
            if (!$sReady2.WaitOne(20000) -or $sProc2.HasExited) { throw 'Atrium.Settings.exe failed second smoke startup.' }
            $hwnd2 = [IntPtr]::Zero
            Wait-Until 10000 {
                $script:hwndTmp2 = [AtriumVerify.UiaSmoke]::FindWindowByProp($sProc2.Id, 'Atrium.Settings.Window')
                $script:hwndTmp2 -ne [IntPtr]::Zero
            } 'Atrium.Settings.exe window handle not found on second launch.'
            $hwnd2 = $script:hwndTmp2
            Wait-Until 10000 {
                ([AtriumVerify.UiaSmoke]::GetToggleState($hwnd2, 'ClockSecondsSwitch')) -eq 1
            } 'ClockSecondsSwitch did not persist On (1) state on second launch.'
            if (![AtriumVerify.UiaSmoke]::PostClose($hwnd2)) { throw 'PostMessage(WM_CLOSE) failed on second Settings window.' }
            if (!$sProc2.WaitForExit(15000) -or $sProc2.ExitCode -ne 0) {
                throw "Second Atrium.Settings.exe did not exit cleanly (exited=$($sProc2.HasExited), exitCode=$($sProc2.ExitCode))."
            }
            $exit = Start-Process -FilePath $core -ArgumentList '--exit' -WorkingDirectory $temp -WindowStyle Hidden -PassThru
            try {
                $exitDone = $exit.WaitForExit(15000)
                $exitCode = if ($exitDone) { $exit.ExitCode } else { 'timeout' }
                if (!$exitDone -or $exitCode -ne 0 -or !$proc.WaitForExit(15000)) {
                    throw "Atrium.exe --exit failed (exitCode=$exitCode, exited=$($proc.HasExited))."
                }
            } finally {
                $exit.Dispose()
            }
            if (!(Test-Path -LiteralPath $logPath -PathType Leaf)) { throw "Missing core.log: $logPath" }
            $logDelta = Read-LogDelta $logPath $initialLogLen
            if ($logDelta -notmatch 'core: starting' -or $logDelta -notmatch 'core: exited') {
                throw "core.log missing expected startup/exit lines.`nDelta:`n$logDelta"
            }
            if ($logDelta -match 'render thread stuck') {
                throw "core.log reported render thread stuck.`nDelta:`n$logDelta"
            }
        } catch {
            $coreDelta = Read-LogDelta $logPath $initialLogLen
            $settingsDelta = Read-LogDelta $settingsLogPath $initialSettingsLogLen
            throw "$($_.Exception.Message)`ncore.log delta:`n$coreDelta`nsettings.log delta:`n$settingsDelta"
        } finally {
            foreach ($sp in $settingsProcs) {
                if (!$sp.HasExited) {
                    Stop-Process -Id $sp.Id -Force -ErrorAction SilentlyContinue
                    [void]$sp.WaitForExit(3000)
                }
                $sp.Dispose()
            }
            foreach ($wh in $settingsWaitHandles) { $wh.Dispose() }
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
