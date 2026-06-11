namespace Crymore.Loader.Helpers;

public static class UpdateLog
{
    private static readonly string[] Entries =
    [
        "Jun 2026 · wpf loader ui",
        "May 2026 · native loader redesign",
        "Apr 2026 · overlay menu refresh",
        "Mar 2026 · env check improvements",
        "Feb 2026 · payload pipeline update",
        "Jan 2026 · initial crymore release",
    ];

    public static string Formatted => string.Join('\n', Entries);
}
