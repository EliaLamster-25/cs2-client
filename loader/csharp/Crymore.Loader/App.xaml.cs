using System.Windows;
using System.Windows.Threading;
using Crymore.Loader.Helpers;

namespace Crymore.Loader;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        DebugLog.Init();

        AppDomain.CurrentDomain.UnhandledException += (_, args) =>
        {
            if (args.ExceptionObject is Exception ex)
                DebugLog.WriteException("Unhandled AppDomain", ex);
            else
                DebugLog.Write($"Unhandled AppDomain: {args.ExceptionObject}");
        };

        DispatcherUnhandledException += (_, args) =>
        {
            DebugLog.WriteException("Dispatcher unhandled", args.Exception);
            args.Handled = true;
            MessageBox.Show(
                $"The loader hit an error.\n\n{args.Exception.Message}\n\nDetails were written to:\n{DebugLog.Path}",
                "crymore loader",
                MessageBoxButton.OK,
                MessageBoxImage.Error);
        };

        TaskScheduler.UnobservedTaskException += (_, args) =>
        {
            DebugLog.WriteException("Unobserved task", args.Exception);
            args.SetObserved();
        };

        base.OnStartup(e);
    }
}
