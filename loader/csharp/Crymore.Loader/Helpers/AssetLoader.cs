using System.Windows.Media;

namespace Crymore.Loader.Helpers;

public static class AssetLoader
{
    public static ImageSource? Logo { get; private set; }

    public static void Init()
    {
        Logo = ResourceImages.FromPack("Assets/logo.png");
    }
}
