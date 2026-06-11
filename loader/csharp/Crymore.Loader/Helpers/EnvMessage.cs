namespace Crymore.Loader.Helpers;

using Crymore.Loader.Models;

public static class EnvMessage
{
    public static string FromErrors(IReadOnlyList<string> errors)
    {
        if (errors.Count == 0)
            return "Environment check failed.";

        var first = Sanitize(errors[0]);
        if (first.Contains("FACEIT", StringComparison.OrdinalIgnoreCase))
            return "Disable/stop FACEIT AC.";
        if (first.Contains("Vanguard", StringComparison.OrdinalIgnoreCase) ||
            first.Contains("Riot", StringComparison.OrdinalIgnoreCase))
            return "Disable Riot Vanguard.";
        if (first.Contains("Easy Anti-Cheat", StringComparison.OrdinalIgnoreCase))
            return "Close Easy Anti-Cheat.";
        if (first.Contains("BattlEye", StringComparison.OrdinalIgnoreCase))
            return "Close BattlEye service.";
        if (first.Contains("ESEA", StringComparison.OrdinalIgnoreCase))
            return "Close ESEA client.";
        return first;
    }

    public static IEnumerable<string> FilterLogs(IEnumerable<string> lines, bool verbose = false)
    {
        var list = lines
            .Select(Sanitize)
            .Where(l => !string.IsNullOrWhiteSpace(l))
            .ToList();

        if (verbose)
            return list.TakeLast(30);

        return list.Where(l =>
            l.StartsWith("[launch]", StringComparison.Ordinal) ||
            l.StartsWith("[env]", StringComparison.Ordinal) ||
            l.StartsWith("[warn]", StringComparison.Ordinal) ||
            l.StartsWith("[auth]", StringComparison.Ordinal) ||
            l.StartsWith("[boot]", StringComparison.Ordinal) ||
            l.StartsWith("[prefetch]", StringComparison.Ordinal) ||
            l.StartsWith("[error]", StringComparison.Ordinal))
            .TakeLast(20);
    }

    public static string LogFileHint(string? nativePath)
    {
        return "See loader.log in LocalAppData\\crymore";
    }

    public static string FormatLaunchProgress(LoaderState state)
    {
        if (!string.IsNullOrWhiteSpace(state.Status))
            return Sanitize(state.Status);

        return state.LaunchStep switch
        {
            "steam" => "Starting Steam and CS2...",
            "download" => "Downloading overlay...",
            "decrypt" => "Decrypting overlay...",
            "extract" => "Extracting files...",
            "prepare" => "Preparing...",
            _ => "Starting payload...",
        };
    }

    private static string Sanitize(string line)
    {
        if (string.IsNullOrWhiteSpace(line))
            return "";

        line = line
            .Replace('\u2026', '.')
            .Replace('\u2014', '-')
            .Replace('\u2013', '-')
            .Replace('\u00B7', '-')
            .Replace("â€¦", "...")
            .Replace("â€”", "-")
            .Replace("â€“", "-");

        if (line.Contains("[debug]", StringComparison.Ordinal))
            return "";
        if (line.Contains("path=", StringComparison.OrdinalIgnoreCase))
            return "";
        if (line.Contains("log=", StringComparison.OrdinalIgnoreCase))
            return "";

        var i = 0;
        while (i < line.Length - 2)
        {
            if (i > 0 && char.IsLetter(line[i - 1]) && line[i] == ':' &&
                (line[i + 1] == '\\' || line[i + 1] == '/'))
            {
                var start = i - 1;
                var end = i + 2;
                while (end < line.Length && line[end] != ' ')
                    end++;
                line = line.Remove(start, end - start);
                i = Math.Max(0, start);
                continue;
            }
            i++;
        }

        return line.Trim();
    }

    private static string FormatMb(ulong bytes) =>
        $"{bytes / (1024.0 * 1024.0):0.0} MB";
}
