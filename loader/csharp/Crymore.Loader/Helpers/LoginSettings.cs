using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace Crymore.Loader.Helpers;

public sealed class LoginSettings
{
    public bool RememberMe { get; set; }
    public string Username { get; set; } = "";
    public string Password { get; set; } = "";
}

public static class LoginSettingsStore
{
    private static string Path => System.IO.Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "crymore", "login.json");

    public static LoginSettings Load()
    {
        try
        {
            if (!File.Exists(Path))
                return new LoginSettings();
            var json = File.ReadAllText(Path);
            var settings = JsonSerializer.Deserialize<LoginSettings>(json) ?? new LoginSettings();
            settings.Password = Unprotect(settings.Password);
            return settings;
        }
        catch
        {
            return new LoginSettings();
        }
    }

    public static void Save(LoginSettings settings)
    {
        try
        {
            var dir = System.IO.Path.GetDirectoryName(Path)!;
            Directory.CreateDirectory(dir);
            if (!settings.RememberMe)
            {
                if (File.Exists(Path))
                    File.Delete(Path);
                return;
            }

            var stored = new LoginSettings
            {
                RememberMe = settings.RememberMe,
                Username = settings.Username,
                Password = Protect(settings.Password)
            };
            var json = JsonSerializer.Serialize(stored);
            File.WriteAllText(Path, json);
        }
        catch { /* best effort */ }
    }

    private static string Protect(string plain)
    {
        if (string.IsNullOrEmpty(plain))
            return "";
        var bytes = ProtectedData.Protect(Encoding.UTF8.GetBytes(plain), null, DataProtectionScope.CurrentUser);
        return Convert.ToBase64String(bytes);
    }

    private static string Unprotect(string stored)
    {
        if (string.IsNullOrEmpty(stored))
            return "";
        try
        {
            var bytes = ProtectedData.Unprotect(Convert.FromBase64String(stored), null, DataProtectionScope.CurrentUser);
            return Encoding.UTF8.GetString(bytes);
        }
        catch
        {
            return "";
        }
    }
}
