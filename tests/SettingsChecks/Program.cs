using System.Diagnostics;
using System.Globalization;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using Atrium.Settings;

static void Require(bool condition, string message)
{
    if (!condition) throw new InvalidOperationException(message);
}

var settings = new BarSettings
{
    DockDisplayMode = "all",
    DockDisplayDelay = 750,
    DockDisplay = @"\\?\DISPLAY#external",
    DesktopDisplay = "disconnected-screen",
    TaskbarApps = "primaryAndWindow",
    TaskbarRightPrimaryOnly = true,
    NotifyPlacement = "topRight",
    HoverRise = false,
    NearEnter = 40,
    NearLeave = 4,
};
var parsed = BarSettings.FromJson(settings.ToJson(), new());
Require(settings.SameAs(parsed), "Display preferences must survive a settings round trip.");
Require(parsed.NearEnter == 40 && parsed.NearLeave == 4,
    "A four-DIP leave setting must survive a round trip independently of enter.");
Require(BarSettings.FromJson(JsonNode.Parse("""{"nearLeave":4}""")!.AsObject(), new()).NearLeave == 4,
    "A partial four-DIP leave update must not be replaced with the default forty-eight.");
Require(BarSettings.FromJson([], new()).SameAs(new()), "Old files must receive display defaults.");
Require(new BarSettings().ImeStatus, "The input method status is on by default.");
var imeOff = BarSettings.FromJson(JsonNode.Parse("""{"imeStatus":false}""")!.AsObject(), new());
Require(!imeOff.ImeStatus && !new BarSettings().SameAs(imeOff) && !BarSettings.FromJson(imeOff.ToJson(), new()).ImeStatus,
    "Turning the input method status off must be saved, compared and survive a round trip.");
Require(!BarSettings.FromJson([], new()).HoverRise, "Old files must keep the glass flat.");
Require(BarSettings.FromJson([], new() { HoverRise = true }).HoverRise, "Partial updates must retain enabled hover rise.");
Require(!BarSettings.FromJson([], settings).HoverRise, "Partial updates must retain disabled hover rise.");
Require(!BarSettings.FromJson(JsonNode.Parse("""{"hoverRise":"bad"}""")!.AsObject(), settings).HoverRise,
    "Malformed hover rise must retain the current value.");
parsed = BarSettings.FromJson(JsonNode.Parse("""{"hoverRise":true}""")!.AsObject(), settings);
Require(parsed.HoverRise && parsed.Magnify == settings.Magnify, "Hover rise must toggle without changing icon magnification.");
Require(BarSettings.FromJson([], new()).StartStatus && BarSettings.FromJson([], new()).StartTray,
    "Old files must keep the start menu's status and tray buttons.");
parsed = BarSettings.FromJson(JsonNode.Parse("""{"startStatus":false}""")!.AsObject(), new());
Require(!parsed.StartStatus && parsed.StartTray && BarSettings.FromJson(parsed.ToJson(), new()).SameAs(parsed),
    "The start menu's status buttons must turn off on their own and survive a round trip.");
Require(!new BarSettings().SameAs(parsed), "The start menu's status buttons must participate in the Save/Revert dirty state.");
Require(!BarSettings.FromJson([], new()).StageMinimize, "Old files must keep minimized windows out of Stage Manager.");
parsed = BarSettings.FromJson(JsonNode.Parse("""{"stageMinimize":true}""")!.AsObject(), new());
Require(parsed.StageMinimize && BarSettings.FromJson(parsed.ToJson(), new()).StageMinimize && !new BarSettings().SameAs(parsed),
    "Minimizing to Stage Manager must survive a round trip and mark the settings dirty.");
var excludedDefault = new BarSettings();
Require(excludedDefault.StageExcluded.SequenceEqual(BarSettings.StageExcludedDefaults) &&
        excludedDefault.StageExcluded.Contains("snippingtool.exe") && excludedDefault.StageExcluded.Contains("microsoft.screensketch_") &&
        !excludedDefault.StageExcluded.Contains("wechat.exe") && !excludedDefault.StageExcluded.Contains("notepad.exe"),
    "The Stage Manager blacklist must start with the Windows screenshot tools.");
var excluded = BarSettings.FromJson(JsonNode.Parse("""{"stageExcluded":["wechat.exe","tencent.qq_"]}""")!.AsObject(), new());
Require(excluded.StageExcluded.SequenceEqual(["wechat.exe", "tencent.qq_"]) &&
        BarSettings.FromJson(excluded.ToJson(), new()).SameAs(excluded) && !new BarSettings().SameAs(excluded),
    "The Stage Manager blacklist must survive a round trip and mark the settings dirty.");
var excludedEmpty = BarSettings.FromJson(JsonNode.Parse("""{"stageExcluded":[]}""")!.AsObject(), new());
Require(excludedEmpty.StageExcluded.Count == 0 && BarSettings.FromJson(excludedEmpty.ToJson(), new()).SameAs(excludedEmpty),
    "An empty Stage Manager blacklist must stay empty and survive a round trip.");
var excludedMalformed = BarSettings.FromJson(JsonNode.Parse("""{"stageExcluded":false}""")!.AsObject(), excluded);
Require(excludedMalformed.StageExcluded.SequenceEqual(excluded.StageExcluded),
    "A malformed Stage Manager blacklist must retain the current list.");
Require(!BarSettings.FromJson([], new()).ShowClockSeconds, "Old files must keep the clock at minutes.");
Require(!new BarSettings().MagnifyNear && !BarSettings.FromJson([], new()).MagnifyNear,
    "Icons must not magnify above the bar unless chosen.");
Require(!new BarSettings().ShowDesktop && BarSettings.FromJson(JsonNode.Parse("""{"showDesktop":true}""")!.AsObject(), new()).ShowDesktop &&
    !new BarSettings().SameAs(BarSettings.FromJson(JsonNode.Parse("""{"showDesktop":true}""")!.AsObject(), new())),
    "The Show Desktop slot is off by default and turning it on is saved.");
Require(new BarSettings().TrayHost && BarSettings.FromJson([], new()).TrayHost, "Atrium hosts the tray icons unless turned off.");
{
    var old = BarSettings.FromJson([], new());
    var chosen = BarSettings.FromJson(
        JsonNode.Parse("""{"attentionLight":false,"attentionBounce":true,"startCentered":true,"startWithBar":true}""")!.AsObject(), new());
    Require(old.AttentionLight && !old.AttentionBounce && !old.StartCentered && !old.StartWithBar,
        "Old files light apps asking for attention, without hops, the start menu at the left, alone.");
    Require(!chosen.AttentionLight && chosen.AttentionBounce && chosen.StartCentered && chosen.StartWithBar && !chosen.SameAs(old) &&
            BarSettings.FromJson(JsonNode.Parse(chosen.ToJson().ToJsonString())!.AsObject(), new()).SameAs(chosen),
        "The attention and start menu choices are read, compared and saved.");
}
parsed = BarSettings.FromJson(JsonNode.Parse("""{"trayHost":false}""")!.AsObject(), new());
Require(!parsed.TrayHost && !BarSettings.FromJson(parsed.ToJson(), new()).TrayHost && !new BarSettings().SameAs(parsed),
    "Turning the tray host off must survive a round trip and mark the settings dirty.");
parsed = BarSettings.FromJson(JsonNode.Parse("""{"remoteVisible":true}""")!.AsObject(), new());
Require(parsed.RemoteVisible && BarSettings.FromJson(parsed.ToJson(), new()).RemoteVisible && !new BarSettings().SameAs(parsed),
    "Remote-control mode must survive a round trip and mark the settings dirty.");
parsed = BarSettings.FromJson(JsonNode.Parse("""{"showClockSeconds":true}""")!.AsObject(), new());
Require(parsed.ShowClockSeconds && parsed.ShowClock &&
        BarSettings.FromJson(parsed.ToJson(), new()).SameAs(parsed) && !new BarSettings().SameAs(parsed),
    "Clock seconds must survive a round trip, mark the settings dirty and leave the clock on.");
Require(BarSettings.FromJson(JsonNode.Parse("""{"showClockSeconds":"bad"}""")!.AsObject(),
        new() { ShowClockSeconds = true }).ShowClockSeconds,
    "Malformed clock seconds must retain the current value.");
Require(!BarSettings.FromJson([], new()).TaskbarRightPrimaryOnly, "Old files must keep right-side elements on every display.");
Require(BarSettings.FromJson([], settings).TaskbarRightPrimaryOnly, "Partial updates must retain primary-only visibility.");
Require(BarSettings.FromJson(JsonNode.Parse("""{"taskbarRightPrimaryOnly":"bad"}""")!.AsObject(), settings).TaskbarRightPrimaryOnly,
    "Malformed primary-only visibility must retain the current value.");
parsed = BarSettings.FromJson(JsonNode.Parse("""{"taskbarRightPrimaryOnly":false}""")!.AsObject(), settings);
Require(!parsed.TaskbarRightPrimaryOnly && parsed.ShowTray && parsed.ShowStatus && parsed.ShowClock && parsed.ShowNotifications,
    "Disabling primary-only visibility must retain the individual content switches.");
Require(BarSettings.FromJson([], new()).NotifyPlacement == "dock", "Existing users must retain the default notification placement.");
Require(BarSettings.FromJson([], new()).Language == "auto", "Old files must follow the system language.");
settings.Language = "en";
Require(BarSettings.FromJson(settings.ToJson(), new()).Language == "en", "The interface language must survive a round trip.");
Require(BarSettings.FromJson(JsonNode.Parse("""{"language":"bad tag"}""")!.AsObject(), settings).Language == "auto",
    "Invalid languages must follow the system.");
Require(BarSettings.FromJson(JsonNode.Parse("""{"language":false}""")!.AsObject(), settings).Language == "en",
    "Malformed languages must retain the current value.");
Require(BarSettings.FromJson(JsonNode.Parse("""{"notifyPlacement":"bad"}""")!.AsObject(), new()).NotifyPlacement == "dock",
    "Invalid notification placement must normalize.");
Require(BarSettings.FromJson(JsonNode.Parse("""{"notifyPlacement":false}""")!.AsObject(), settings).NotifyPlacement == "topRight",
    "Malformed notification placement must retain the current value.");
parsed = BarSettings.FromJson(JsonNode.Parse("""{"dockDisplayMode":"bad","taskbarApps":"bad","dockDisplayDelay":99999}""")!.AsObject(), new());
Require(parsed.DockDisplayMode == "primary" && parsed.TaskbarApps == "all" && parsed.DockDisplayDelay == 2000,
    "Invalid values must normalize like the native core.");
parsed = BarSettings.FromJson(JsonNode.Parse("""{"dockDisplayDelay":-1}""")!.AsObject(), new());
Require(parsed.DockDisplayDelay == 100, "Delay must have a lower bound.");
parsed = BarSettings.FromJson(JsonNode.Parse("""{"dockDisplayDelay":"bad","desktopDisplay":false}""")!.AsObject(), settings);
Require(parsed.DockDisplayDelay == settings.DockDisplayDelay && parsed.DesktopDisplay == settings.DesktopDisplay,
    "Malformed values must retain the current settings.");
var original = settings.Clone();
settings.ApplyMaterial(BarSettings.Frosted());
Require(settings.DockDisplayMode == original.DockDisplayMode && settings.DockDisplay == original.DockDisplay &&
    settings.DesktopDisplay == original.DesktopDisplay && settings.DockDisplayDelay == original.DockDisplayDelay &&
    settings.TaskbarApps == original.TaskbarApps && settings.TaskbarRightPrimaryOnly == original.TaskbarRightPrimaryOnly &&
    settings.NotifyPlacement == original.NotifyPlacement && settings.HoverRise == original.HoverRise &&
    settings.WindowsContextMenu == original.WindowsContextMenu,
    "Material presets must preserve monitor, notification and hover-rise choices.");
foreach (var edit in new Action<BarSettings>[]
{
    s => s.DockDisplayMode = "fixed", s => s.DockDisplayDelay += 50, s => s.DockDisplay += "other",
    s => s.DesktopDisplay += "other", s => s.TaskbarApps = "all", s => s.NotifyPlacement = "dock",
    s => s.TaskbarRightPrimaryOnly = false,
    s => s.HoverRise = true, s => s.NearEnter += 2, s => s.NearLeave += 2,
})
{
    var changed = settings.Clone();
    edit(changed);
    Require(!settings.SameAs(changed), "Every display preference must participate in the Save/Revert dirty state.");
}
var menuOff = BarSettings.FromJson(JsonNode.Parse("""{"windowsContextMenu":false}""")!.AsObject(), new());
Require(!menuOff.WindowsContextMenu && BarSettings.FromJson(menuOff.ToJson(), new()).SameAs(menuOff),
    "The Windows-style context menu must turn off and survive a round trip.");
Require(new BarSettings().WindowsContextMenu && BarSettings.FromJson([], new()).WindowsContextMenu,
    "The Windows-style context menu is on by default, also for files written before it existed.");
Require(!new BarSettings().SameAs(menuOff), "The Windows-style context menu must participate in the Save/Revert dirty state.");
var displays = DesktopDisplay.Read();
Require(displays.Count > 0 && displays.Any(d => d.Primary), "Connected displays and system primary must be discoverable.");
Require(displays.All(d => d.Id.Length > 0 && d.Scale > 0 && d.Width > 0 && d.WorkWidth > 0), "Display IDs, geometry and scale must be valid.");
var shortcutPin = new PinnedApp("Browser", @"C:\Links\Browser.lnk", "", "");
Require(!shortcutPin.AllowsMultipleWindows() && shortcutPin.AllowsMultipleWindows(@"C:\Program Files\chrome.exe"),
    "PinnedApp multiple-window default must use the resolved target path without shell I/O.");
Require(!(shortcutPin with { MultipleWindows = false }).AllowsMultipleWindows(@"C:\Program Files\chrome.exe"),
    "An explicit launch-mode override must take precedence over the resolved target path.");
Require(new PinnedApp("Edge", @"C:\Program Files\msedge.exe", "", "").AllowsMultipleWindows() &&
        !new PinnedApp("Notepad", @"C:\Windows\notepad.exe", "", "").AllowsMultipleWindows(),
    "An .exe pin with no resolved path must fall back to its own executable name.");
Require(new PinnedApp("Explorer", "", "", "Microsoft.Windows.Explorer").AllowsMultipleWindows(),
    "The Explorer AUMID must default to allowing multiple windows.");
Console.WriteLine($"Settings round trips, validation, dirty tracking and display discovery passed ({displays.Count} connected display(s)).");

string? Arg(string flag) => Array.IndexOf(args, flag) is int idx && idx >= 0 && idx + 1 < args.Length ? args[idx + 1] : null;
string? coreNormalizer = Arg("--core-normalizer"), repoRoot = Arg("--repo-root");
for (string? d = AppContext.BaseDirectory; repoRoot == null && d != null; d = Directory.GetParent(d)?.FullName)
{
    if (File.Exists(Path.Combine(d, "docs", "SETTINGS-PROTOCOL.md")) &&
        File.Exists(Path.Combine(d, "tests", "fixtures", "settings-schema.json")))
    { repoRoot = d; }
}
Require(repoRoot != null, "Could not locate repository root.");
string buildRoot = Path.Combine(repoRoot!, "native", "build");
string normalizerPath = coreNormalizer != null
    ? (File.Exists(coreNormalizer) ? coreNormalizer : throw new FileNotFoundException($"Core normalizer not found: {coreNormalizer}"))
    : BuildConfigs.SelectMany(c => new[]
        { Path.Combine(buildRoot, c, "tests", "display_settings_tests.exe"), Path.Combine(buildRoot, c, "tests", "Release", "display_settings_tests.exe") })
        .Where(File.Exists).OrderByDescending(File.GetLastWriteTimeUtc).FirstOrDefault()
        ?? throw new FileNotFoundException($"display_settings_tests.exe not found under {buildRoot}.");

var schemaKeys = JsonNode.Parse(File.ReadAllText(Path.Combine(repoRoot!, "tests", "fixtures", "settings-schema.json")))!["keys"]!.AsObject();
string protocolText = File.ReadAllText(Path.Combine(repoRoot!, "docs", "SETTINGS-PROTOCOL.md"));
string protocolTable = protocolText[..protocolText.IndexOf("## 管道", StringComparison.Ordinal)];
var docKeys = DocKeyRegex().Matches(protocolTable).Select(m => m.Groups[1].Value).ToHashSet(StringComparer.Ordinal);
var schemaKeySet = schemaKeys.Select(kv => kv.Key).ToHashSet(StringComparer.Ordinal);
var csKeys = new BarSettings { StartSize = "medium", StartIcon = "medium", Pins = [] }.ToJson().Select(kv => kv.Key).ToHashSet(StringComparer.Ordinal);

var altBase = new JsonObject();
foreach (var (k, node) in schemaKeys)
{
    var s = node!.AsObject();
    altBase[k] = s["type"]!.GetValue<string>() switch
    {
        "number" => Math.Abs(s["default"]!.GetValue<double>() - s["min"]!.GetValue<double>()) > 1e-6
            ? s["min"]!.GetValue<double>() : s["max"]!.GetValue<double>(),
        "bool" => !s["default"]!.GetValue<bool>(),
        "enum" => k == "startSize" ? "large" : k == "startIcon" ? "small"
            : s["values"]!.AsArray().Select(v => v!.GetValue<string>()).First(v => v != s["default"]!.GetValue<string>()),
        "string" => @"\\?\DISPLAY#alt",
        "tint" => "#123456",
        "language" => "en",
        "stringArray" => new JsonArray("a.exe"),
        "pins" => new JsonArray(new JsonObject { ["name"] = "App", ["path"] = @"C:\app.exe", ["args"] = "--x", ["aumid"] = "", ["multipleWindows"] = true }),
        _ => throw new InvalidOperationException($"Unknown schema type for {k}")
    };
}
var altBaseSettings = BarSettings.FromJson(altBase, new());

var cases = new List<(string Label, JsonObject? BaseObj, BarSettings BaseSettings, JsonObject? PatchObj, string? RawPatch)>
{
    ("keys:populated", null, new(), JsonNode.Parse("""{"startSize":"medium","startIcon":"medium","pins":[]}""")!.AsObject(), null),
    ("empty:default", null, new(), new(), null), ("empty:alt", altBase, altBaseSettings, new(), null),
    ("v1:migration", null, new(), JsonNode.Parse("""{"version":1,"notifyListener":false}""")!.AsObject(), null),
    ("v2:no-migration", null, new(), JsonNode.Parse("""{"version":2,"notifyListener":false}""")!.AsObject(), null),
    ("malformed:json", null, new(), null, "{broken"),
};
foreach (var (k, node) in schemaKeys)
{
    var s = node!.AsObject();
    void Add(string sub, JsonNode? val) => cases.Add(($"{k}:{sub}", altBase, altBaseSettings, new JsonObject { [k] = val }, null));
    switch (s["type"]!.GetValue<string>())
    {
        case "number":
            double lo = s["min"]!.GetValue<double>(), hi = s["max"]!.GetValue<double>();
            Add("min", lo); Add("max", hi); Add("under", lo - 100); Add("over", hi + 100);
            Add("str", "bad"); Add("bool", false); Add("null", null); Add("arr", new JsonArray()); break;
        case "bool": Add("true", true); Add("false", false); Add("str", "true"); Add("num", 1); Add("null", null); break;
        case "enum":
            foreach (var v in s["values"]!.AsArray()) Add($"val:{v!.GetValue<string>()}", v.GetValue<string>());
            Add("unknown", "__unknown__"); Add("empty", ""); Add("bool", false); Add("num", 123); Add("null", null); break;
        case "string": Add("empty", ""); Add("display", @"\\?\DISPLAY#1"); Add("bool", false); break;
        case "tint": foreach (string t in TintSamples) Add($"val:{t}", t); Add("bool", false); break;
        case "language": foreach (string l in LangSamples) Add($"val:{l}", l); Add("bool", false); break;
        case "stringArray":
            Add("empty", new JsonArray()); Add("ws", new JsonArray(" wechat.exe ", "  ", "foo_"));
            Add("bool", false); Add("mixed", new JsonArray(123, "ok.exe")); break;
        case "pins":
            Add("empty", new JsonArray());
            var over65 = new JsonArray();
            for (int i = 0; i < 65; i++) { over65.Add(new JsonObject { ["name"] = $"P{i}", ["path"] = $@"C:\p{i}.exe" }); }
            Add("65", over65);
            Add("drop-empty", new JsonArray(new JsonObject { ["name"] = "D", ["path"] = "" }, new JsonObject { ["name"] = "K", ["path"] = @"C:\k.exe" }));
            Add("multi", new JsonArray(new JsonObject { ["name"] = "A", ["path"] = @"C:\a.exe", ["multipleWindows"] = true },
                new JsonObject { ["name"] = "B", ["path"] = @"C:\b.exe", ["multipleWindows"] = false },
                new JsonObject { ["name"] = "C", ["path"] = @"C:\c.exe" }));
            Add("bad", "bad"); break;
    }
}

var psi = new ProcessStartInfo(normalizerPath, "--normalize");
psi.RedirectStandardInput = psi.RedirectStandardOutput = true;
psi.StandardInputEncoding = psi.StandardOutputEncoding = new System.Text.UTF8Encoding(false);
using var proc = Process.Start(psi)!;
var readTask = Task.Run(() =>
{
    var l = new List<string>();
    while (proc.StandardOutput.ReadLine() is string s) { if (!string.IsNullOrWhiteSpace(s)) l.Add(s); }
    return l;
});
foreach (var (_, baseObj, _, patchObj, rawPatch) in cases)
{
    var req = new JsonObject();
    if (baseObj != null) req["base"] = baseObj.DeepClone();
    if (rawPatch != null) req["patchText"] = rawPatch; else req["patch"] = patchObj!.DeepClone();
    proc.StandardInput.WriteLine(req.ToJsonString());
}
proc.StandardInput.Close();
var timeout = TimeSpan.FromSeconds(15);
List<string> respLines;
try
{
    respLines = await readTask.WaitAsync(timeout);
    await proc.WaitForExitAsync().WaitAsync(timeout);
}
catch (TimeoutException ex)
{
    try { proc.Kill(entireProcessTree: true); } catch (InvalidOperationException) { }
    throw new TimeoutException($"display_settings_tests --normalize timed out after {timeout.TotalSeconds:0}s.", ex);
}
Require(proc.ExitCode == 0, $"Core normalizer exited with code {proc.ExitCode}.");
Require(respLines.Count == cases.Count, $"Expected {cases.Count} normalizer responses, got {respLines.Count}.");

static bool JsonEqual(JsonNode? a, JsonNode? b)
{
    if (a is null || b is null) return a is null && b is null;
    if (a is JsonObject ao && b is JsonObject bo)
        return ao.Count == bo.Count && ao.All(kv => bo.TryGetPropertyValue(kv.Key, out var bv) && JsonEqual(kv.Value, bv));
    if (a is JsonArray aa && b is JsonArray ba)
        return aa.Count == ba.Count && aa.Zip(ba, JsonEqual).All(x => x);
    if (a is JsonValue av && b is JsonValue bv)
    {
        if (av.TryGetValue(out bool ab) && bv.TryGetValue(out bool bb)) return ab == bb;
        if (av.TryGetValue(out string? astr) && bv.TryGetValue(out string? bstr)) return astr == bstr;
        if (av.GetValueKind() == System.Text.Json.JsonValueKind.Number && bv.GetValueKind() == System.Text.Json.JsonValueKind.Number)
        {
            double ad = double.Parse(av.ToJsonString(), System.Globalization.CultureInfo.InvariantCulture);
            double bd = double.Parse(bv.ToJsonString(), System.Globalization.CultureInfo.InvariantCulture);
            return Math.Abs(ad - bd) <= 1e-9;
        }
    }
    return false;
}

static void AssertSchemaCompliance(string label, JsonObject cs, JsonObject schemaKeys)
{
    foreach (var (k, node) in schemaKeys)
    {
        var s = node!.AsObject();
        string type = s["type"]!.GetValue<string>();
        bool hasKey = cs.TryGetPropertyValue(k, out var val);
        if (label == "empty:default")
        {
            bool canOmit = s["omitWhenEmpty"]?.GetValue<bool>() == true || s["optional"]?.GetValue<bool>() == true;
            Require((hasKey || canOmit) && (!hasKey || JsonEqual(val, s["default"])),
                $"Default mismatch [{label}] for {k}: got {val?.ToJsonString() ?? "<omitted>"}");
        }
        bool ok = type switch
        {
            "number" => hasKey && val is JsonValue nv && nv.GetValueKind() == JsonValueKind.Number &&
                double.Parse(nv.ToJsonString(), CultureInfo.InvariantCulture) is double d &&
                d >= s["min"]!.GetValue<double>() && d <= s["max"]!.GetValue<double>(),
            "enum" => (!hasKey || val is null)
                ? s["omitWhenEmpty"]?.GetValue<bool>() == true
                : val is JsonValue ev && ev.TryGetValue(out string? es) &&
                    ((s["omitWhenEmpty"]?.GetValue<bool>() == true && es == "") ||
                     s["values"]!.AsArray().Any(v => v!.GetValue<string>() == es)),
            "tint" => hasKey && val is JsonValue tv && tv.TryGetValue(out string? ts) && TintRegex().IsMatch(ts),
            "bool" => hasKey && val is JsonValue bv && bv.TryGetValue(out bool _),
            "string" or "language" => hasKey && val is JsonValue sv && sv.TryGetValue(out string? _),
            "stringArray" => hasKey && val is JsonArray sa && sa.All(e => e is JsonValue jv && jv.TryGetValue(out string? _)),
            "pins" => (!hasKey || val is null)
                ? s["optional"]?.GetValue<bool>() == true
                : val is JsonArray pa && pa.Count <= (s["maxCount"]?.GetValue<int>() ?? 64),
            _ => throw new InvalidOperationException($"Unknown schema type {type} for {k}")
        };
        Require(ok, $"Schema violation [{label}] for {k} ({type}): {val?.ToJsonString() ?? "<omitted>"}");
    }
}

for (int i = 0; i < cases.Count; i++)
{
    var (label, _, baseSettings, patchObj, rawPatch) = cases[i];
    var resp = JsonNode.Parse(respLines[i])!.AsObject();
    if (rawPatch != null) { Require(!resp["ok"]!.GetValue<bool>(), $"Malformed JSON [{label}] must return ok=false."); continue; }
    Require(resp["ok"]!.GetValue<bool>(), $"Case [{label}] failed in C++ normalizer.");
    var cpp = resp["result"]!.AsObject();
    if (label == "keys:populated")
    {
        var cppKeys = cpp.Select(kv => kv.Key).ToHashSet(StringComparer.Ordinal);
        void CheckSet(string name, HashSet<string> actual)
        {
            if (actual.SetEquals(schemaKeySet)) return;
            throw new InvalidOperationException($"4-way key mismatch ({name} vs schema): " +
                $"missing=[{string.Join(",", schemaKeySet.Except(actual))}], extra=[{string.Join(",", actual.Except(schemaKeySet))}]");
        }
        CheckSet("SETTINGS-PROTOCOL.md", docKeys); CheckSet("BarSettings.ToJson", csKeys); CheckSet("Settings.ToJson", cppKeys);
    }
    var cs = BarSettings.FromJson(patchObj, baseSettings).ToJson();
    if (!JsonEqual(cs, cpp))
        throw new InvalidOperationException($"Differential mismatch [{label}]\nC#:  {cs.ToJsonString()}\nC++: {cpp.ToJsonString()}");
    AssertSchemaCompliance(label, cs, schemaKeys);
    if (label == "pins:65") Require(cs["pins"]!.AsArray().Count == 64, "Pins must cap at 64.");
    if (label == "v1:migration") Require(cs["notifyListener"]!.GetValue<bool>(), "v1 migration must keep notifyListener=true.");
    if (label == "v2:no-migration") Require(!cs["notifyListener"]!.GetValue<bool>(), "v2 must apply notifyListener=false.");
}

string xamlCs = File.ReadAllText(Path.Combine(repoRoot!, "settings", "Atrium.Settings", "MainWindow.xaml.cs"));
var sliderMatches = SliderRegex().Matches(xamlCs);
var schemaSliders = schemaKeys.Where(kv => kv.Value!["sliderProp"] != null)
    .ToDictionary(kv => kv.Value!["sliderProp"]!.GetValue<string>(), kv => (kv.Key, Spec: kv.Value!.AsObject()));
Require(sliderMatches.Count == schemaSliders.Count, $"Expected {schemaSliders.Count} sliders in MainWindow.xaml.cs, found {sliderMatches.Count}.");
foreach (Match m in sliderMatches)
{
    double uiMin = double.Parse(m.Groups[1].Value, System.Globalization.CultureInfo.InvariantCulture);
    double uiMax = double.Parse(m.Groups[2].Value, System.Globalization.CultureInfo.InvariantCulture);
    string prop = m.Groups[3].Value;
    Require(schemaSliders.TryGetValue(prop, out var entry), $"Slider property {prop} missing from schema.");
    var sp = entry.Spec;
    double sMin = sp["uiMin"]!.GetValue<double>(), sMax = sp["uiMax"]!.GetValue<double>();
    double scale = sp["uiScale"]?.GetValue<double>() ?? 1, mMin = sp["min"]!.GetValue<double>(), mMax = sp["max"]!.GetValue<double>();
    Require(uiMin == sMin && uiMax == sMax, $"Slider {prop} ({entry.Key}) UI [{uiMin},{uiMax}] != schema [{sMin},{sMax}].");
    Require(uiMin / scale >= mMin && uiMax / scale <= mMax, $"Slider {prop} ({entry.Key}) scaled exceeds model [{mMin},{mMax}].");
}
Console.WriteLine($"Settings contract passed: {schemaKeySet.Count} keys (4-way), " +
    $"{cases.Count} C++<->C# differential cases, {sliderMatches.Count} sliders aligned.");

static void ExpectThrows<TException>(Action action, string message) where TException : Exception
{
    try { action(); }
    catch (TException) { return; }
    throw new InvalidOperationException(message);
}

var libPaths = GameFinder.ParseLibraryPaths(""" "path" "D:\\SteamLibrary" "path" "E:\\Games\\Steam" """);
Require(libPaths.SequenceEqual([@"D:\SteamLibrary", @"E:\Games\Steam"]), "VDF library paths must unescape.");
string acf = """ "name" "Portal 2" "installdir" "Portal 2" """;
Require(GameFinder.ParseVdfField(acf, "name") == "Portal 2" && GameFinder.ParseVdfField(acf, "installdir") == "Portal 2",
    "ACF fields must parse.");
Require(GameFinder.IsExcludedRuntime("Steamworks Common Redistributables") &&
        GameFinder.IsExcludedRuntime("Foo Redistributable") && !GameFinder.IsExcludedRuntime("Portal 2"),
    "Steam runtime exclusion must filter runtimes only.");
var epicValid = GameFinder.ParseEpicManifest(
    """{"AppCategories":["games"],"DisplayName":"Hades","InstallLocation":"C:\\Games\\Hades","LaunchExecutable":"Hades.exe"}""");
Require(epicValid is { Name: "Hades", Folder: @"C:\Games\Hades", LaunchExe: "Hades.exe" }, "Valid Epic manifest must parse.");
Require(GameFinder.ParseEpicManifest("""{"AppCategories":["engines"],"DisplayName":"UE","InstallLocation":"C:\\UE"}""") is null,
    "Non-game Epic manifest must be ignored.");
Require(GameFinder.ParseEpicManifest("{bad") is null, "Malformed Epic manifest must return null.");
var merged = GameFinder.MergeGames([
    new("Portal 2", @"d:\steam\common\portal 2\", "a.exe"),
    new("Portal 2", @"D:\Steam\common\Portal 2\portal2.exe", "b.exe"),
    new("Notepad", @"c:\windows\notepad.exe", "n.exe"),
]);
Require(merged.Count == 2 && merged[0].Title == "Notepad" && merged[1].Title == "Portal 2",
    "MergeGames must deduplicate exe entries inside game folders.");

static JsonElement ReleaseJson(string tag, string? assetName = null, string? digest = null)
{
    var assets = new JsonArray();
    if (assetName != null)
    {
        var asset = new JsonObject { ["name"] = assetName, ["browser_download_url"] = "https://example.com/a.zip" };
        if (digest != null) asset["digest"] = digest;
        assets.Add(asset);
    }
    var obj = new JsonObject { ["tag_name"] = tag, ["html_url"] = "https://example.com/rel", ["assets"] = assets };
    return JsonDocument.Parse(obj.ToJsonString()).RootElement;
}
var relOk = ReleaseInfo.FromJson(ReleaseJson("v2.7.0", "Atrium-2.7.0-win-x64.zip", "sha256:abc"));
Require(relOk.Version == new Version(2, 7, 0, 0) && relOk.Download != null && relOk.Digest == "sha256:abc",
    "ReleaseInfo.FromJson must parse v2.7.0, matching zip asset, and digest.");
Require(ReleaseInfo.FromJson(ReleaseJson("v2.7.0+build")).Version == new Version(2, 7, 0, 0) &&
        ReleaseInfo.FromJson(ReleaseJson("v2.7.0", "other.zip")).Download == null,
    "ReleaseInfo.FromJson must handle +build tags and missing matching asset.");
ExpectThrows<JsonException>(() => ReleaseInfo.FromJson(ReleaseJson("bad")), "Invalid version must throw JsonException.");
ExpectThrows<KeyNotFoundException>(() => ReleaseInfo.FromJson(JsonDocument.Parse("{}").RootElement), "Missing tag_name must throw.");

string temp = Path.Combine(Path.GetTempPath(), "atrium-checks-" + Guid.NewGuid().ToString("N"));
try
{
    Directory.CreateDirectory(temp);
    string goodZip = Path.Combine(temp, "good.zip");
    using (var z = ZipFile.Open(goodZip, ZipArchiveMode.Create))
    {
        var e = z.CreateEntry("Atrium/ok.txt");
        using var w = new StreamWriter(e.Open());
        w.Write("ok");
    }
    string goodDigest = "sha256:" + Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(goodZip)));
    ReleaseValidation.VerifyZipDigest(goodZip, goodDigest);
    ExpectThrows<IOException>(() => ReleaseValidation.VerifyZipDigest(goodZip, "sha256:0000"), "Digest mismatch must throw.");
    string extractDir = Path.Combine(temp, "out");
    ReleaseValidation.ExtractZipSafely(goodZip, extractDir);
    Require(File.ReadAllText(Path.Combine(extractDir, "Atrium", "ok.txt")) == "ok", "ExtractZipSafely must extract valid zip.");
    string slipZip = Path.Combine(temp, "slip.zip");
    using (var z = ZipFile.Open(slipZip, ZipArchiveMode.Create))
        z.CreateEntry("../escape.txt");
    ExpectThrows<InvalidDataException>(() => ReleaseValidation.ExtractZipSafely(slipZip, extractDir), "ZipSlip must be rejected.");

    string pkg = Path.Combine(temp, "pkg");
    string coreExe = Path.Combine(pkg, "Atrium.exe");
    string settingsExe = Path.Combine(pkg, "Settings", "Atrium.Settings.exe");
    string settingsDll = Path.Combine(pkg, "Settings", "Atrium.Settings.dll");
    Directory.CreateDirectory(Path.GetDirectoryName(settingsExe)!);
    File.WriteAllText(coreExe, ""); File.WriteAllText(settingsExe, ""); File.WriteAllText(settingsDll, "");
    Version v27 = new(2, 7, 0, 0), v26 = new(2, 6, 0, 0);
    ReleaseValidation.ValidatePackage(pkg, v27, _ => v27);
    ExpectThrows<IOException>(() => ReleaseValidation.ValidatePackage(pkg, v27, _ => v26), "Version mismatch must throw.");
    File.Delete(settingsDll);
    ExpectThrows<IOException>(() => ReleaseValidation.ValidatePackage(pkg, v27, _ => v27), "Missing Atrium.Settings.dll must throw.");
    File.Delete(coreExe);
    ExpectThrows<IOException>(() => ReleaseValidation.ValidatePackage(pkg, v27, _ => v27), "Missing Atrium.exe must throw.");

    string cacheDir = Path.Combine(temp, "cache");
    string stateFile = Path.Combine(cacheDir, "ready.json");
    string workDir = Path.Combine(cacheDir, "package-background");
    Directory.CreateDirectory(workDir);
    ReleaseValidation.SaveJsonAtomic(stateFile, new { Tag = "v2.8.0", Version = "2.8.0" });
    Require(File.Exists(stateFile) && Directory.GetFiles(cacheDir, "*.tmp").Length == 0,
        "SaveJsonAtomic must write target without leaving temporary files.");
    ReleaseValidation.SaveJsonAtomic(stateFile, new { Tag = "v2.9.0", Version = "2.9.0" });
    Require(JsonNode.Parse(File.ReadAllText(stateFile))!["Version"]!.GetValue<string>() == "2.9.0",
        "SaveJsonAtomic must overwrite existing JSON atomically.");
    Require(ReleaseValidation.TryRecoverPrepared(() => "ok", stateFile, workDir) == "ok" && File.Exists(stateFile),
        "TryRecoverPrepared must reuse valid prepared package.");
    foreach (var ex in new Exception[]
    {
        new JsonException("corrupt"),
        new IOException("missing"),
        new FormatException("bad version"),
        new InvalidDataException("bad zip"),
        new ArgumentException("bad path"),
    })
    {
        Directory.CreateDirectory(workDir);
        File.WriteAllText(Path.Combine(workDir, "stale.txt"), "x");
        File.WriteAllText(stateFile, "{corrupt");
        Exception? logged = null;
        string? recovered = ReleaseValidation.TryRecoverPrepared<string>(
            () => throw ex, stateFile, workDir, e => logged = e);
        Require(recovered == null && ReferenceEquals(logged, ex) && !File.Exists(stateFile) && !Directory.Exists(workDir),
            $"TryRecoverPrepared must log and clean up on {ex.GetType().Name}.");
    }
    Directory.CreateDirectory(workDir);
    File.WriteAllText(stateFile, "{}");
    ExpectThrows<UnauthorizedAccessException>(
        () => ReleaseValidation.TryRecoverPrepared<string>(() => throw new UnauthorizedAccessException("denied"), stateFile, workDir),
        "TryRecoverPrepared must not swallow UnauthorizedAccessException.");

    string mutexName = @"Local\Atrium.Update.Test." + Guid.NewGuid().ToString("N");
    string resultFile = Path.Combine(cacheDir, "result.json");
    Require(ReleaseValidation.ReadFailureWithLock(resultFile, mutexName) == null,
        "ReadFailureWithLock must return null when result.json is absent.");
    ReleaseValidation.SaveJsonAtomic(resultFile, new { error = "boom" });
    Require(ReleaseValidation.ReadFailureWithLock(resultFile, mutexName) == "boom" && !File.Exists(resultFile),
        "ReadFailureWithLock must return error and delete result.json.");
    ReleaseValidation.SaveJsonAtomic(resultFile, new { error = "" });
    Require(ReleaseValidation.ReadFailureWithLock(resultFile, mutexName) == null && !File.Exists(resultFile),
        "ReadFailureWithLock must return null on empty error and delete result.json.");
    File.WriteAllText(resultFile, "{broken");
    Exception? corruptLogged = null;
    Require(ReleaseValidation.ReadFailureWithLock(resultFile, mutexName, e => corruptLogged = e) == null &&
            corruptLogged is JsonException && !File.Exists(resultFile),
        "ReadFailureWithLock must log, clean up corrupt result.json, and not throw.");

    Require(ShellIcons.ExecutableForIcon("foo.exe") == "foo.exe", "Bare exe must return itself.");
    Require(ShellIcons.ExecutableForIcon(@"C:\app\foo.lnk") == @"C:\app\foo.lnk", "Non-exe must return itself.");
    string iconExe = Path.Combine(temp, "MyApp.exe");
    string appIconExe = Path.Combine(temp, "app", "MyApp.exe");
    Require(ShellIcons.ExecutableForIcon(iconExe) == iconExe, "Missing app/<exe> must return original path.");
    Directory.CreateDirectory(Path.GetDirectoryName(appIconExe)!);
    File.WriteAllText(appIconExe, "");
    Require(ShellIcons.ExecutableForIcon(iconExe) == appIconExe, "Existing app/<exe> must be returned.");
}
finally
{
    if (Directory.Exists(temp)) Directory.Delete(temp, true);
}

{
    string cppFacesH = File.ReadAllText(Path.Combine(repoRoot!, "native", "core", "ui", "widget_faces.h"));
    string cppFaces = File.ReadAllText(Path.Combine(repoRoot!, "native", "core", "ui", "widget_faces.cpp"));
    string csFaces = File.ReadAllText(Path.Combine(repoRoot!, "settings", "Atrium.Settings", "WidgetFaces.cs"));
    string cppWidgets = File.ReadAllText(Path.Combine(repoRoot!, "native", "core", "ui", "desktop_widgets.cpp"));
    var zhHans = JsonNode.Parse(File.ReadAllText(Path.Combine(repoRoot!, "locales", "zh-Hans.json")))!["strings"]!.AsObject();
    static string Slice(string src, string marker)
    {
        int start = src.IndexOf(marker, StringComparison.Ordinal);
        int end = src.IndexOf("};", start, StringComparison.Ordinal);
        int csEnd = src.IndexOf("];", start, StringComparison.Ordinal);
        if (end < 0 || (csEnd >= 0 && csEnd < end)) end = csEnd;
        return src[start..end];
    }
    var cppCities = CppCityRegex().Matches(Slice(cppFaces, "kCities ="))
        .Select(m => (Name: m.Groups[1].Value, Zone: m.Groups[2].Value)).ToList();
    var cppLegacy = CppLegacyRegex().Matches(Slice(cppFacesH, "kLegacy[] =")).ToDictionary(m => m.Groups[2].Value, m => m.Groups[1].Value);
    var csCities = CsCityRegex().Matches(Slice(csFaces, "WorldCities ="))
        .Select(m => (Name: m.Groups[1].Value, Zone: m.Groups[2].Value, Legacy: m.Groups[3].Value)).ToList();
    Require(cppCities.Count == 29 && csCities.Count == 29 &&
            cppCities.Zip(csCities).All(p => p.First.Name == p.Second.Name && p.First.Zone == p.Second.Zone &&
                                             cppLegacy.TryGetValue(p.Second.Name, out string? leg) && leg == p.Second.Legacy),
        "WorldCities in widget_faces.{h,cpp} and WidgetFaces.cs must have 29 identical entries.");
    Require(zhHans["Local"]?.GetValue<string>() == "本地" &&
            csCities.All(c => zhHans[c.Name]?.GetValue<string>() == c.Legacy),
        "Every canonical WorldCities key and Local must match locales/zh-Hans.json.");
    Require(cppFaces.Contains("""{L"Tokyo", L"London", L"New York"}""") &&
            cppFaces.Contains("""{L"Beijing", L"Tokyo", L"London", L"New York"}""") &&
            csFaces.Contains("""["Tokyo", "London", "New York"]""") &&
            csFaces.Contains("""["Beijing", "Tokyo", "London", "New York"]"""),
        "Default world clock cities must match between C++ and C#.");
    string Canonical(string s) => csCities.FirstOrDefault(c => c.Name == s || c.Legacy == s).Name ?? s;
    Require(Canonical("北京") == "Beijing" && Canonical("圣保罗") == "São Paulo" && Canonical("奥克兰") == "Auckland" &&
            Canonical("Beijing") == "Beijing" && Canonical("Unknown") == "Unknown",
        "Legacy Chinese city keys must normalize to canonical English keys.");
    Require(cppWidgets.Contains("CanonicalCity(") && csFaces.Contains("CanonicalCity("),
        "Both C++ and C# must normalize legacy city keys on load.");
}
Console.WriteLine("Hardening regression checks passed.");

internal partial class Program
{
    private static readonly string[] BuildConfigs = ["win-x64", "tests", "development", "tidy"];
    private static readonly string[] TintSamples = ["#000000", "#abcdef", "#123456", "# 12345", "#GGGGGG", "123456"];
    private static readonly string[] LangSamples = ["auto", "zh", "zh-Hans", "en", "fr-CA", "-en", "en--US", "bad tag", "中文"];
    [GeneratedRegex(@"^\|\s*`(\w+)`\s*\|", RegexOptions.Multiline)]
    private static partial Regex DocKeyRegex();
    [GeneratedRegex(@"^#[0-9a-fA-F]{6}$")]
    private static partial Regex TintRegex();
    [GeneratedRegex(@"new\(""[^""]+"",\s*Loc\.T\(""[^""]+""\),\s*([0-9.]+),\s*([0-9.]+),\s*[0-9.]+,\s*""[^""]*"",\s*s\s*=>\s*s\.(\w+)")]
    private static partial Regex SliderRegex();
    [GeneratedRegex(@"\{L""([^""]+)"",\s*L""([^""]+)""\}")]
    private static partial Regex CppCityRegex();
    [GeneratedRegex(@"\{L""([^""]+)"",\s*L""([^""]+)""\}")]
    private static partial Regex CppLegacyRegex();
    [GeneratedRegex(@"\(""([^""]+)"",\s*""([^""]+)"",\s*""([^""]+)""\)")]
    private static partial Regex CsCityRegex();
}
