using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Crymore.Loader.Helpers;
using Crymore.Loader.Models;

namespace Crymore.Loader.Services;

public static class CoreBridge
{
    private const string DllName = "crymore_core.dll";

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    private static extern int crymore_init(IntPtr hwnd);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    private static extern void crymore_shutdown();

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    private static extern void crymore_tick(float dt);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    private static extern void crymore_login(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string username,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string password);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    private static extern void crymore_launch();

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    private static extern void crymore_logout();

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    private static extern void crymore_close();

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    private static extern int crymore_get_state_json(StringBuilder buf, int capacity);

    public static void Init(IntPtr hwnd)
    {
        if (crymore_init(hwnd) == 0)
            throw new InvalidOperationException("crymore_init failed.");
    }

    public static void Shutdown() => crymore_shutdown();

    public static void Tick(float dt) => crymore_tick(dt);

    public static void Login(string user, string pass) => crymore_login(user, pass);

    public static void Launch() => crymore_launch();

    public static void Logout() => crymore_logout();

    public static void Close() => crymore_close();

    public static LoaderState GetState()
    {
        try
        {
            var need = crymore_get_state_json(new StringBuilder(0), 0);
            if (need <= 0)
                return new LoaderState();

            var sb = new StringBuilder(need + 1);
            crymore_get_state_json(sb, sb.Capacity);
            return JsonSerializer.Deserialize<LoaderState>(sb.ToString()) ?? new LoaderState();
        }
        catch (Exception ex)
        {
            DebugLog.WriteException("GetState", ex);
            return new LoaderState
            {
                Phase = "failed",
                Status = "Could not read loader state — see loader.log",
                Logs = ["[error] UI failed to read native state"],
                LogPath = DebugLog.Path
            };
        }
    }
}
