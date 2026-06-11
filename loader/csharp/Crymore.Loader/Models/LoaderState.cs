using System.Text.Json.Serialization;

namespace Crymore.Loader.Models;

public sealed class LoaderState
{
    [JsonPropertyName("phase")]
    public string Phase { get; set; } = "login";

    [JsonPropertyName("loggedIn")]
    public bool LoggedIn { get; set; }

    [JsonPropertyName("envOk")]
    public bool EnvOk { get; set; }

    [JsonPropertyName("busy")]
    public bool Busy { get; set; }

    [JsonPropertyName("status")]
    public string Status { get; set; } = "";

    [JsonPropertyName("username")]
    public string Username { get; set; } = "";

    [JsonPropertyName("plan")]
    public string Plan { get; set; } = "";

    [JsonPropertyName("expiresAt")]
    public string ExpiresAt { get; set; } = "";

    [JsonPropertyName("avatarUrl")]
    public string AvatarUrl { get; set; } = "";

    [JsonPropertyName("subscriptionDaysRemaining")]
    public int SubscriptionDaysRemaining { get; set; }

    [JsonPropertyName("envErrors")]
    public List<string> EnvErrors { get; set; } = [];

    [JsonPropertyName("logs")]
    public List<string> Logs { get; set; } = [];

    [JsonPropertyName("logPath")]
    public string LogPath { get; set; } = "";

    [JsonPropertyName("launchStep")]
    public string LaunchStep { get; set; } = "";

    [JsonPropertyName("downloadBytes")]
    public ulong DownloadBytes { get; set; }

    [JsonPropertyName("downloadTotal")]
    public ulong DownloadTotal { get; set; }

    [JsonPropertyName("prefetchBusy")]
    public bool PrefetchBusy { get; set; }

    [JsonPropertyName("prefetchReady")]
    public bool PrefetchReady { get; set; }

    [JsonPropertyName("pendingLaunch")]
    public bool PendingLaunch { get; set; }
}
