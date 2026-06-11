using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using Crymore.Loader.Helpers;
using Crymore.Loader.Models;
using Crymore.Loader.Services;

namespace Crymore.Loader;

public partial class MainWindow : Window
{
    private const double LoginW = 380;
    private const double LoginH = 560;
    private const double MainW = 1000;
    private const double MainH = 560;
    private const double CornerR = 8;
    private const string UsernamePlaceholder = "username";

    private readonly DispatcherTimer _timer;
    private bool _wasLoggedIn;
    private Storyboard? _pulseStoryboard;
    private DateTime _lastTick = DateTime.UtcNow;

    public MainWindow()
    {
        InitializeComponent();
        Loaded += OnLoaded;
        Closed += OnClosed;

        Width = LoginW;
        Height = LoginH;

        UsernameBox.GotFocus += (_, _) => ClearPlaceholder(UsernameBox, UsernamePlaceholder);
        UsernameBox.LostFocus += (_, _) => RestorePlaceholder(UsernameBox, UsernamePlaceholder);
        UsernameBox.Text = UsernamePlaceholder;
        UsernameBox.Foreground = (Brush)FindResource("DimBrush");

        PasswordInput.GotFocus += (_, _) => PasswordWatermark.Visibility = Visibility.Collapsed;
        PasswordInput.LostFocus += (_, _) => UpdatePasswordWatermark();
        PasswordInput.PasswordChanged += (_, _) => UpdatePasswordWatermark();
        ShowPasswordPlaceholder();

        _timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(33) };
        _timer.Tick += (_, _) => OnFrame();
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        AssetLoader.Init();
        AvatarLoader.Init();
        ApplyLogo();
        ApplyWindowIcon();
        Title = $"crymore.pw {BuildInfo.Display}";
        TitleLabel.Text = $"crymore.pw {BuildInfo.Display}";
        BuildVersionLabel.Text = BuildInfo.Display;
        LoadRememberedLogin();
        UpdateLogText.Text = UpdateLog.Formatted;
        UpdateRoundClip();

        var hwnd = new System.Windows.Interop.WindowInteropHelper(this).Handle;
        CoreBridge.Init(hwnd);
        _timer.Start();
        AnimatePageIn(LoginPage);
    }

    private void LoadRememberedLogin()
    {
        var saved = LoginSettingsStore.Load();
        if (!saved.RememberMe)
            return;

        RememberMeBox.IsChecked = true;

        if (!string.IsNullOrWhiteSpace(saved.Username))
        {
            UsernameBox.Text = saved.Username;
            UsernameBox.Foreground = (Brush)FindResource("InkBrush");
        }

        if (!string.IsNullOrWhiteSpace(saved.Password))
            SetPassword(saved.Password);
    }

    private void ShellBorder_SizeChanged(object sender, SizeChangedEventArgs e) => UpdateRoundClip();

    private void UpdateRoundClip()
    {
        if (ShellBorder.ActualWidth <= 0 || ShellBorder.ActualHeight <= 0)
            return;
        ShellBorder.Clip = new RectangleGeometry(
            new Rect(0, 0, ShellBorder.ActualWidth, ShellBorder.ActualHeight),
            CornerR, CornerR);
    }

    private void ApplyLogo()
    {
        var logo = AssetLoader.Logo;
        if (logo is null)
            return;

        LogoMain.Source = logo;
        LogoBlurMid.Source = logo;
        LogoBlurLarge.Source = logo;
        SideLogo.Source = logo;
    }

    private void ApplyWindowIcon()
    {
        try
        {
            var uri = new Uri("pack://application:,,,/Assets/app.ico", UriKind.Absolute);
            Icon = BitmapFrame.Create(uri);
        }
        catch { /* exe ApplicationIcon still used by shell */ }
    }

    private void OnClosed(object? sender, EventArgs e)
    {
        _timer.Stop();
        _pulseStoryboard?.Stop();
        CoreBridge.Shutdown();
    }

    private void OnFrame()
    {
        try
        {
            var now = DateTime.UtcNow;
            var dt = (float)(now - _lastTick).TotalSeconds;
            _lastTick = now;

            CoreBridge.Tick(dt);
            ApplyState(CoreBridge.GetState());
        }
        catch (Exception ex)
        {
            DebugLog.WriteException("OnFrame", ex);
            LoginErrorText.Text = "Loader error - see log file";
            LogText.Text = EnvMessage.LogFileHint(null) + "\n" + ex.Message;
        }
    }

    private string? _lastAvatarUrl;

    private void ApplyState(LoaderState state)
    {
        if (state.LoggedIn != _wasLoggedIn)
        {
            if (state.LoggedIn)
            {
                if (RememberMeBox.IsChecked == true)
                {
                    LoginSettingsStore.Save(new LoginSettings
                    {
                        RememberMe = true,
                        Username = UsernameValue,
                        Password = PasswordValue
                    });
                }

                TransitionToMain();
                LoginPage.Visibility = Visibility.Collapsed;
                StatusPage.Visibility = Visibility.Visible;
                AnimatePageIn(StatusPage);
            }
            else
            {
                TransitionToLogin();
                StatusPage.Visibility = Visibility.Collapsed;
                LoginPage.Visibility = Visibility.Visible;
                AnimatePageIn(LoginPage);
            }
            _wasLoggedIn = state.LoggedIn;
        }

        if (!state.LoggedIn)
        {
            LoginButton.IsEnabled = !state.Busy;
            LoginButton.Content = state.Busy ? "checking..." : "login";

            if (!string.IsNullOrWhiteSpace(state.Status) &&
                !state.Status.Contains("Sign in", StringComparison.OrdinalIgnoreCase))
                LoginErrorText.Text = state.Status;
            else
                LoginErrorText.Text = "";
            return;
        }

        ProfileUser.Text = string.IsNullOrWhiteSpace(state.Username) ? "user" : state.Username;
        var plan = string.IsNullOrWhiteSpace(state.Plan) ? "free" : state.Plan;
        if (state.SubscriptionDaysRemaining > 0)
            ProfilePlan.Text = $"{plan} - {state.SubscriptionDaysRemaining}d left";
        else
            ProfilePlan.Text = plan;
        ProfileExpiry.Text = string.IsNullOrWhiteSpace(state.ExpiresAt)
            ? "-"
            : FormatExpiry(state.ExpiresAt);

        UpdateProfileAvatar(state.AvatarUrl);

        var phase = state.Phase;
        var queuedInject = state.PendingLaunch && state.PrefetchBusy && !state.PrefetchReady;
        var injecting = phase is "launching" or "done" || queuedInject;
        var loaded = phase is "done";
        var failed = phase is "failed";
        var envOk = state.EnvOk;

        SetStep(DotLoggedIn, TxtLoggedIn, true, true);
        SetStep(DotLoading, TxtLoading, injecting, loaded || failed);
        SetStep(DotLoaded, TxtLoaded, loaded, loaded);

        PulseDot(DotLoading, injecting && !loaded && !failed);

        if (!envOk)
        {
            LaunchTitle.Text = "blocked";
            LaunchDesc.Text = EnvMessage.FromErrors(state.EnvErrors);
            LaunchDesc.Foreground = (Brush)FindResource("BadBrush");
            LaunchDot.Fill = (Brush)FindResource("BadBrush");
        }
        else if (queuedInject)
        {
            LaunchTitle.Text = "ready";
            LaunchDesc.Text = "downloading files...";
            LaunchDesc.Foreground = (Brush)FindResource("MutedBrush");
            LaunchDot.Fill = (Brush)FindResource("WarnBrush");
            PulseDot(LaunchDot, true);
        }
        else if (injecting && !loaded)
        {
            LaunchTitle.Text = "loading";
            LaunchDesc.Text = EnvMessage.FormatLaunchProgress(state);
            LaunchDesc.Foreground = (Brush)FindResource("MutedBrush");
            LaunchDot.Fill = (Brush)FindResource("WarnBrush");
            PulseDot(LaunchDot, true);
        }
        else if (loaded)
        {
            LaunchTitle.Text = "loaded";
            LaunchDesc.Text = "overlay running";
            LaunchDesc.Foreground = (Brush)FindResource("MutedBrush");
            LaunchDot.Fill = (Brush)FindResource("OkBrush");
        }
        else if (failed)
        {
            LaunchTitle.Text = "failed";
            var detail = string.IsNullOrWhiteSpace(state.Status) ? "see log" : state.Status;
            LaunchDesc.Text = detail + "\n" + EnvMessage.LogFileHint(null);
            LaunchDesc.Foreground = (Brush)FindResource("BadBrush");
            LaunchDot.Fill = (Brush)FindResource("BadBrush");
        }
        else
        {
            LaunchTitle.Text = "ready";
            LaunchDesc.Text = state.PrefetchBusy
                ? "downloading files..."
                : state.PrefetchReady
                    ? "ready to inject"
                    : $"checks passed - loader {BuildInfo.Display}";
            LaunchDesc.Foreground = (Brush)FindResource("MutedBrush");
            LaunchDot.Fill = (Brush)FindResource("OkBrush");
        }

        var isLaunching = phase is "launching";
        var isInjecting = isLaunching || queuedInject;
        var canInject = envOk && phase is "ready" && !queuedInject && !isLaunching;

        InjectButton.IsEnabled = canInject;
        InjectLabel.Text = queuedInject ? "downloading..." : isLaunching ? "injecting..." : phase is "done" ? "running" : "inject";

        if (queuedInject || isLaunching)
        {
            InjectRing.Visibility = Visibility.Visible;
            var downloadActive = state.DownloadTotal > 0 && state.DownloadBytes < state.DownloadTotal;
            InjectRing.Progress = downloadActive
                ? state.DownloadBytes / (double)state.DownloadTotal
                : double.NaN;
        }
        else
        {
            InjectRing.Visibility = Visibility.Collapsed;
            InjectRing.Progress = double.NaN;
        }

        LogText.Text = string.Join('\n', EnvMessage.FilterLogs(state.Logs, failed || !envOk));
        if (failed || !envOk)
            LogText.Text += "\n" + EnvMessage.LogFileHint(null);
    }

    private void UpdateProfileAvatar(string? url)
    {
        if (string.IsNullOrWhiteSpace(url))
        {
            ProfileLogo.Source = AvatarLoader.Placeholder ?? AssetLoader.Logo;
            _lastAvatarUrl = null;
            return;
        }

        if (url == _lastAvatarUrl)
            return;

        _lastAvatarUrl = url;
        ProfileLogo.Source = AvatarLoader.Placeholder ?? AssetLoader.Logo;
        _ = LoadAvatarAsync(url);
    }

    private async Task LoadAvatarAsync(string url)
    {
        var img = await AvatarLoader.LoadAsync(url);
        if (img is null || url != _lastAvatarUrl)
            return;
        await Dispatcher.InvokeAsync(() => ProfileLogo.Source = img);
    }

    private static string FormatExpiry(string iso)
    {
        if (DateTime.TryParse(iso, out var dt))
            return dt.ToLocalTime().ToString("g");
        return iso;
    }

    private void TransitionToMain() => AnimateWindowSize(MainW, MainH);

    private void TransitionToLogin() => AnimateWindowSize(LoginW, LoginH);

    private void AnimateWindowSize(double targetW, double targetH)
    {
        var left = Left + (Width - targetW) * 0.5;
        var top = Top + (Height - targetH) * 0.5;

        var duration = TimeSpan.FromMilliseconds(320);
        var ease = new QuadraticEase { EasingMode = EasingMode.EaseInOut };

        Animate(this, WidthProperty, targetW, duration, ease);
        Animate(this, HeightProperty, targetH, duration, ease);
        Animate(this, LeftProperty, left, duration, ease);
        Animate(this, TopProperty, top, duration, ease);
    }

    private static void Animate(Window target, DependencyProperty prop,
        double to, TimeSpan duration, IEasingFunction ease)
    {
        var anim = new DoubleAnimation(to, duration) { EasingFunction = ease };
        target.BeginAnimation(prop, anim);
    }

    private void SetStep(System.Windows.Shapes.Ellipse dot, System.Windows.Controls.TextBlock label,
        bool active, bool done)
    {
        if (done)
        {
            dot.Fill = (Brush)FindResource("OkBrush");
            label.Foreground = (Brush)FindResource("InkBrush");
            label.FontWeight = FontWeights.Medium;
        }
        else if (active)
        {
            dot.Fill = (Brush)FindResource("WarnBrush");
            label.Foreground = (Brush)FindResource("InkBrush");
            label.FontWeight = FontWeights.Normal;
        }
        else
        {
            dot.Fill = (Brush)FindResource("DimBrush");
            label.Foreground = (Brush)FindResource("MutedBrush");
            label.FontWeight = FontWeights.Normal;
        }
    }

    private void PulseDot(System.Windows.Shapes.Ellipse dot, bool pulse)
    {
        if (pulse)
        {
            _pulseStoryboard ??= (Storyboard)FindResource("PulseStoryboard");
            if (_pulseStoryboard.Children.Count > 0)
            {
                Storyboard.SetTarget(_pulseStoryboard.Children[0], dot);
                _pulseStoryboard.Begin(dot, true);
            }
        }
        else
        {
            dot.BeginAnimation(OpacityProperty, null);
            dot.Opacity = 1;
        }
    }

    private void AnimatePageIn(FrameworkElement page)
    {
        page.Opacity = 0;
        page.RenderTransform = new TranslateTransform(0, 10);
        var sb = (Storyboard)FindResource("PageInStoryboard");
        sb.Begin(page);
    }

    private static void ClearPlaceholder(TextBox box, string placeholder)
    {
        if (box.Text == placeholder)
        {
            box.Text = "";
            box.Foreground = (Brush)box.FindResource("InkBrush");
        }
    }

    private static void RestorePlaceholder(TextBox box, string placeholder)
    {
        if (string.IsNullOrWhiteSpace(box.Text))
        {
            box.Text = placeholder;
            box.Foreground = (Brush)box.FindResource("DimBrush");
        }
    }

    private void ShowPasswordPlaceholder()
    {
        PasswordInput.Password = "";
        UpdatePasswordWatermark();
    }

    private void UpdatePasswordWatermark()
    {
        PasswordWatermark.Visibility = string.IsNullOrEmpty(PasswordInput.Password)
            ? Visibility.Visible
            : Visibility.Collapsed;
    }

    private void SetPassword(string value)
    {
        PasswordInput.Password = value;
        PasswordWatermark.Visibility = string.IsNullOrEmpty(value)
            ? Visibility.Visible
            : Visibility.Collapsed;
    }

    private string UsernameValue =>
        UsernameBox.Text == UsernamePlaceholder ? "" : UsernameBox.Text.Trim().ToLowerInvariant();

    private string PasswordValue => PasswordInput.Password;

    private void LoginButton_Click(object sender, RoutedEventArgs e)
    {
        LoginErrorText.Text = "";
        if (string.IsNullOrWhiteSpace(UsernameValue) || string.IsNullOrEmpty(PasswordValue))
        {
            LoginErrorText.Text = "enter username and password";
            return;
        }

        CoreBridge.Login(UsernameValue, PasswordValue);
    }

    private void InjectButton_Click(object sender, RoutedEventArgs e)
    {
        DebugLog.Write("Inject button clicked");
        _ = Task.Run(() =>
        {
            try
            {
                CoreBridge.Launch();
            }
            catch (Exception ex)
            {
                DebugLog.WriteException("Inject", ex);
                Dispatcher.Invoke(() =>
                {
                    LaunchTitle.Text = "failed";
                    LaunchDesc.Text = ex.Message + "\n" + EnvMessage.LogFileHint(null);
                    LaunchDesc.Foreground = (Brush)FindResource("BadBrush");
                    LogText.Text = EnvMessage.LogFileHint(null) + "\n" + ex;
                });
            }
        });
    }

    private void CloseButton_Click(object sender, RoutedEventArgs e) => CoreBridge.Close();

    private void TitleBar_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ClickCount == 1)
            DragMove();
    }
}
