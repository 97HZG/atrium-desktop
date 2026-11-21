using System.Text.Json.Nodes;

namespace Atrium.Settings;

// Pure state machine for the Settings window's saved baseline, editable draft,
// optimistic concurrency metadata (InstanceId / Revision), and 3-way merges.
internal sealed class SettingsSession
{
    public BarSettings Saved { get; private set; } = new();
    public BarSettings Draft { get; private set; } = new();
    public string? InstanceId { get; private set; }
    public ulong Revision { get; private set; }
    public long DraftVersion { get; private set; }
    public bool Closed { get; private set; }

    public bool HasUnsavedChanges => !Draft.SameAs(Saved);

    public void MarkEdited()
    {
        if (Closed) return;
        DraftVersion++;
    }

    public void Revert()
    {
        if (Closed) return;
        Draft = Saved.Clone();
        DraftVersion++;
    }

    public void Close() => Closed = true;

    public bool ApplyInitialState(
        BarSettings saved,
        BarSettings current,
        string? instanceId,
        ulong revision,
        BarSettings? pendingSaved = null,
        BarSettings? pendingCurrent = null)
    {
        if (Closed) return false;
        Saved = saved.Clone();
        InstanceId = instanceId;
        Revision = revision;
        if (pendingSaved != null && pendingCurrent != null)
        {
            Draft = MergeThreeWay(pendingSaved, saved, pendingCurrent);
            if (!Draft.SameAs(Saved)) DraftVersion++;
        }
        else
        {
            Draft = current.Clone();
        }
        return HasUnsavedChanges;
    }

    public bool ApplyReconnectSnapshot(
        BarSettings saved,
        BarSettings current,
        string? instanceId,
        ulong revision,
        long draftVersionBeforeAwait)
    {
        if (Closed) return false;
        InstanceId = instanceId;
        Revision = revision;
        bool keepDraft = DraftVersion != draftVersionBeforeAwait || !Draft.SameAs(Saved);
        Draft = keepDraft ? MergeThreeWay(Saved, saved, Draft) : current.Clone();
        Saved = saved.Clone();
        return HasUnsavedChanges;
    }

    public bool ApplyCommitSuccess(BarSettings sent, string? instanceId, ulong revision)
    {
        if (Closed) return false;
        Saved = sent.Clone();
        if (instanceId != null) InstanceId = instanceId;
        if (revision > 0) Revision = revision;
        return true;
    }

    public bool ApplyExternalChange(BarSettings newSaved, ulong revision = 0)
    {
        if (Closed) return false;
        if (revision > 0) Revision = revision;
        if (newSaved.SameAs(Saved)) return false;
        Draft = MergeThreeWay(Saved, newSaved, Draft);
        Saved = newSaved.Clone();
        return true;
    }

    public bool ApplyConflict(BarSettings serverSaved, ulong serverRevision, string? serverInstanceId)
    {
        if (Closed) return false;
        Draft = MergeThreeWay(Saved, serverSaved, Draft);
        Saved = serverSaved.Clone();
        Revision = serverRevision;
        if (serverInstanceId != null) InstanceId = serverInstanceId;
        DraftVersion++;
        return true;
    }

    private static BarSettings MergeThreeWay(BarSettings baseSaved, BarSettings targetSaved, BarSettings draft)
    {
        var baseJson = baseSaved.ToJson();
        var targetJson = targetSaved.ToJson();
        var draftJson = draft.ToJson();
        var merged = new JsonObject();
        var keys = new HashSet<string>(baseJson.Select(kv => kv.Key), StringComparer.Ordinal);
        foreach (var kv in targetJson) keys.Add(kv.Key);
        foreach (var kv in draftJson) keys.Add(kv.Key);
        foreach (string key in keys)
        {
            baseJson.TryGetPropertyValue(key, out var baseVal);
            draftJson.TryGetPropertyValue(key, out var draftVal);
            bool draftChanged = !JsonNode.DeepEquals(draftVal, baseVal);
            var chosen = draftChanged ? draftVal : (targetJson.TryGetPropertyValue(key, out var targetVal) ? targetVal : null);
            if (chosen != null)
                merged[key] = chosen.DeepClone();
        }
        return BarSettings.FromJson(merged, new());
    }
}
