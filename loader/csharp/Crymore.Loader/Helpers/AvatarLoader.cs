using System.IO;
using System.Net.Http;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace Crymore.Loader.Helpers;

public static class AvatarLoader
{
    private static readonly HttpClient Http = new();
    private static string? _lastUrl;

    public static ImageSource? Placeholder { get; private set; }

    public static void Init()
    {
        Placeholder = ResourceImages.FromPack("Assets/picture_placeholder.png");
    }

    public static async Task<ImageSource?> LoadAsync(string? url)
    {
        if (string.IsNullOrWhiteSpace(url))
            return Placeholder;

        if (url == _lastUrl && _cached is not null)
            return _cached;

        try
        {
            var bytes = await Http.GetByteArrayAsync(url);
            await using var ms = new MemoryStream(bytes);
            var img = new BitmapImage();
            img.BeginInit();
            img.CacheOption = BitmapCacheOption.OnLoad;
            img.StreamSource = ms;
            img.EndInit();
            img.Freeze();
            _lastUrl = url;
            _cached = img;
            return img;
        }
        catch
        {
            return Placeholder;
        }
    }

    private static ImageSource? _cached;
}
