using System.Diagnostics;
using System.IO.Compression;
using System.Security.Cryptography;

namespace Atrium.Settings;

internal static class ReleaseValidation
{
    internal static void VerifyZipDigest(string zipPath, string? expectedDigest, Func<string, string>? localize = null)
    {
        if (expectedDigest is null) return;
        using var stream = File.OpenRead(zipPath);
        string actual = "sha256:" + Convert.ToHexString(SHA256.HashData(stream));
        if (!actual.Equals(expectedDigest, StringComparison.OrdinalIgnoreCase))
        {
            string message = "The update package failed verification. Download again.";
            throw new IOException(localize?.Invoke(message) ?? message);
        }
    }

    internal static void ExtractZipSafely(string zipPath, string destination)
    {
        string destRoot = Path.GetFullPath(destination);
        if (!destRoot.EndsWith(Path.DirectorySeparatorChar)) destRoot += Path.DirectorySeparatorChar;
        using var archive = ZipFile.OpenRead(zipPath);
        foreach (var entry in archive.Entries)
        {
            string name = entry.FullName;
            if (Path.IsPathRooted(name) || name.StartsWith('/') || name.StartsWith('\\') ||
                name.Split('/', '\\').Any(segment => segment == ".."))
            {
                throw new InvalidDataException($"Zip entry contains unsafe path: {name}");
            }
            string fullPath = Path.GetFullPath(Path.Combine(destRoot, name));
            if (!fullPath.StartsWith(destRoot, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException($"Zip entry escapes destination: {name}");
        }
        Directory.CreateDirectory(destRoot);
        foreach (var entry in archive.Entries)
        {
            string fullPath = Path.GetFullPath(Path.Combine(destRoot, entry.FullName));
            if (entry.FullName.EndsWith('/') || entry.FullName.EndsWith('\\'))
            {
                Directory.CreateDirectory(fullPath);
            }
            else
            {
                Directory.CreateDirectory(Path.GetDirectoryName(fullPath)!);
                entry.ExtractToFile(fullPath, overwrite: true);
            }
        }
    }

    internal static void ValidatePackage(
        string directory,
        Version expected,
        Func<string, Version>? versionReader = null,
        Func<string, string>? localize = null)
    {
        string core = Path.Combine(directory, "Atrium.exe");
        string settings = Path.Combine(directory, "Settings", "Atrium.Settings.exe");
        string settingsDll = Path.ChangeExtension(settings, ".dll");
        if (!File.Exists(core) || !File.Exists(settings) || !File.Exists(settingsDll))
        {
            string missing = "The update package is missing program files. Download again.";
            throw new IOException(localize?.Invoke(missing) ?? missing);
        }
        versionReader ??= ReadFileVersion;
        if (versionReader(core) != expected || versionReader(settingsDll) != expected)
        {
            string mismatch = "The update package does not match the release. Download again.";
            throw new IOException(localize?.Invoke(mismatch) ?? mismatch);
        }
    }

    private static Version ReadFileVersion(string path)
    {
        var info = FileVersionInfo.GetVersionInfo(path);
        return new Version(info.FileMajorPart, info.FileMinorPart, info.FileBuildPart, info.FilePrivatePart);
    }
}
