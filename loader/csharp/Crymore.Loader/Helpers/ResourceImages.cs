using System.IO;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace Crymore.Loader.Helpers;

public static class ResourceImages
{
    public static ImageSource? FromPack(string packPath)
    {
        try
        {
            var uri = new Uri($"pack://application:,,,/{packPath.TrimStart('/')}", UriKind.Absolute);
            var img = new BitmapImage();
            img.BeginInit();
            img.CacheOption = BitmapCacheOption.OnLoad;
            img.UriSource = uri;
            img.EndInit();
            img.Freeze();
            return img;
        }
        catch
        {
            return null;
        }
    }

    public static ImageSource? FromFileOrPack(string relativeFile, string packPath)
    {
        var path = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, relativeFile));
        if (File.Exists(path) && new FileInfo(path).Length >= 64)
        {
            var img = new BitmapImage();
            img.BeginInit();
            img.CacheOption = BitmapCacheOption.OnLoad;
            img.UriSource = new Uri(path, UriKind.Absolute);
            img.EndInit();
            img.Freeze();
            return img;
        }
        return FromPack(packPath);
    }
}
