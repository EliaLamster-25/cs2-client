using System.IO;
using System.Text;

namespace Crymore.Loader.Helpers;

public static class DebugLog
{
    private static readonly object Gate = new();
    private static string? _path;

    public static string Path =>
        _path ??= System.IO.Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "crymore",
            "loader.log");

    public static void Init()
    {
        try
        {
            var dir = System.IO.Path.GetDirectoryName(Path)!;
            Directory.CreateDirectory(dir);
            Write("=== Crymore.Loader started ===");
        }
        catch
        {
            /* best effort */
        }
    }

    public static void Write(string message)
    {
        try
        {
            lock (Gate)
            {
                var line = $"[{DateTime.Now:yyyy-MM-dd HH:mm:ss}] [ui] {message}";
                File.AppendAllText(Path, line + Environment.NewLine, Encoding.UTF8);
            }
        }
        catch
        {
            /* best effort */
        }
    }

    public static void WriteException(string context, Exception ex)
    {
        Write($"{context}: {ex.GetType().Name}: {ex.Message}");
        if (!string.IsNullOrWhiteSpace(ex.StackTrace))
            Write(ex.StackTrace);
    }
}
